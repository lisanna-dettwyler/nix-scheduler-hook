#include "pbs.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <filesystem>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <thread>
using namespace std::chrono_literals;

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <nix/util/fmt.hh>
#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

#include <pbs_error.h>

static std::string getJobState(int conn, std::string jobId)
{
    attrl attr = {nullptr, ATTR_state, nullptr, nullptr, SET};
    batch_status *status = pbs_statjob(conn, jobId.data(), &attr, "x");
    if (status == nullptr || status->attribs == nullptr)
        throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_state, jobId, pbs_errno));
    std::string value = status->attribs->value;
    pbs_statfree(status);
    return value;
}

static void waitForJobRunning(int conn, std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(conn, jobId);
        if (state == "R")
            return;
        else if (state == "F")
            throw PBSDeletedError(jobId);
        std::this_thread::sleep_for(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

static struct attropl *new_attropl()
{
    return new attropl{nullptr, nullptr, nullptr, nullptr, SET};
}

static void free_attropl_list(struct attropl *at_list)
{
    struct attropl *cur, *tmp;
    for (cur = at_list; cur != NULL; cur = tmp) {
        if (cur->resource != nullptr)
            delete cur->resource;
        if (cur->value != nullptr)
            delete cur->value;
        tmp = cur->next;
        delete cur;
    }
}

PBS::PBS()
{
    if (ourSettings.pbsHost.get().empty())
        connHandle = pbs_connect(nullptr);
    else
        connHandle = pbs_connect(nix::fmt("%s:%u", ourSettings.pbsHost.get(), ourSettings.pbsPort.get()).c_str());
    if (connHandle == -1)
        throw PBSConnectionError(nix::fmt("Error connecting to PBS server: %d", pbs_errno));
}

std::string PBS::submit(nix::StorePath drvPath)
{
    auto jobNameStr = nix::fmt("Nix_Build_%s", std::string(drvPath.to_string()));

    // We don't know the jobdir until after the job is running, so use a
    // relative path for the script generation and update it to an absolute
    // path after submission.
    rootPath = nix::fmt("%s.root", jobNameStr.data());

    char tmp_template[] = "pbsscrptXXXXXX";
    char tmp_name[MAXPATHLEN + 1];
    snprintf(tmp_name, sizeof(tmp_name), "%s/%s", std::filesystem::temp_directory_path().c_str(), tmp_template);
    int fd = mkstemp(tmp_name);
    if (fd == -1)
        throw PBSSubmitError(nix::fmt("Error creating temporary file for PBS script %s", tmp_name));
    __gnu_cxx::stdio_filebuf<char> scriptOutBuf(fd, std::ios::out);
    std::ostream scriptOut(&scriptOutBuf);
    scriptOut << genScript(drvPath, rootPath);
    scriptOut.flush();

    // Attribute chain:
    // v -> k -> N -> (l1/aResBase -> l2 -> l3 -> ...)

    auto store = nix::openStore();
    auto drv = store->readDerivation(drvPath);
    std::string res = "";
    attropl *aResBase = nullptr;
    if (drv.env.count("pbsResources") == 1) {
        json pbsResources = json::parse(drv.env["pbsResources"]);
        attropl *prev = nullptr;
        for (auto & [key, value] : pbsResources.items()) {
            auto attr = new_attropl();
            attr->name = ATTR_l;
            attr->resource = new char[key.size() + 1];
            strncpy(attr->resource, key.data(), key.size());
            attr->resource[key.size()] = '\0';
            std::string strValue(value);
            attr->value = new char[strValue.size() + 1];
            strncpy(attr->value, strValue.data(), strValue.size());
            attr->value[strValue.size()] = '\0';
            if (!aResBase)
                aResBase = attr;
            else if (prev != nullptr)
                prev->next = attr;
            prev = attr;
        }
    }

    attropl aName = {aResBase != nullptr ? aResBase : nullptr, ATTR_N, nullptr, jobNameStr.data(), SET};
    char kfVal[] = "oe";  // Hush write-strings warning
    attropl aKeepFiles = {&aName, ATTR_k, nullptr, kfVal, SET};
    char pathVar[] = PATH_VAR;
    attropl aVariableList = {&aKeepFiles, ATTR_v, nullptr, pathVar, SET};

    char *id = pbs_submit(connHandle, &aVariableList, tmp_name, nullptr, nullptr);
    free_attropl_list(aResBase);
    aName.next = nullptr;
    if (id == nullptr) {
        if (auto err_list = pbs_get_attributes_in_error(connHandle)) {
            auto error = err_list->ecl_attrerr[0];
            throw PBSSubmitError(nix::fmt("Error submitting PBS job: attribute %s is in error: %s", error.ecl_attribute->name, error.ecl_errmsg));
        }
        throw PBSSubmitError(nix::fmt("Error submitting PBS job: %s", pbs_geterrmsg(connHandle)));
    }
    jobId = id;

    waitForJobRunning(connHandle, jobId);

    unlink(tmp_name);

    attrl jobdirAttr = {nullptr, ATTR_jobdir, nullptr, nullptr, SET};
    batch_status *jobdirStatus;
    auto sleepTime = 50ms;
    while (true) {
        jobdirStatus = pbs_statjob(connHandle, jobId.data(), &jobdirAttr, nullptr);
        if (jobdirStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_jobdir, jobId, pbs_errno));
        } else if (jobdirStatus->attribs == nullptr) {
            pbs_statfree(jobdirStatus);
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    std::string jobDir = jobdirStatus->attribs->value;
    pbs_statfree(jobdirStatus);

    auto jobIdNum = nix::tokenizeString<nix::Strings>(jobId, ".").front();
    jobStderr = nix::fmt("%s/%s.e%s", jobDir, jobNameStr, jobIdNum);
    rootPath = nix::fmt("%s/%s.root", jobDir, jobNameStr);

    attrl serverAttr = {nullptr, ATTR_server, nullptr, nullptr, SET};
    sleepTime = 50ms;
    batch_status *serverStatus;
    while (true) {
        serverStatus = pbs_statjob(connHandle, jobId.data(), &serverAttr, nullptr);
        if (serverStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_server, jobId, pbs_errno));
        } else if (serverStatus->attribs == nullptr) {
            pbs_statfree(serverStatus);
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    std::string host = serverStatus->attribs->value;
    pbs_statfree(serverStatus);
    return host;
}

int PBS::waitForJobFinish()
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(connHandle, jobId);
        if (state == "F") {
            attrl exitAttr = {nullptr, ATTR_exit_status, nullptr, nullptr, SET};
            batch_status *exitStatus = pbs_statjob(connHandle, jobId.data(), &exitAttr, "x");
            if (exitStatus == nullptr || exitStatus->attribs == nullptr)
                throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_exit_status, jobId, pbs_errno));
            auto value = std::atoi(exitStatus->attribs->value);
            pbs_statfree(exitStatus);
            return value;
        }
        std::this_thread::sleep_for(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

PBS::~PBS()
{
    if (!jobId.empty())
        pbs_deljob(connHandle, jobId.c_str(), nullptr);

    pbs_disconnect(connHandle);
}
