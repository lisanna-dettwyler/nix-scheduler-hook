#include "pbs.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <filesystem>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <thread>
using namespace std::chrono_literals;

#include <nix/util/fmt.hh>

#include <pbs_error.h>

static std::string getJobState(int conn, std::string jobId)
{
    attrl attr = {nullptr, ATTR_state, nullptr, nullptr, SET};
    batch_status *status = pbs_statjob(conn, jobId.data(), &attr, "x");
    if (status == nullptr) {
        throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_state, jobId, pbs_errno));
    }
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

PBS::PBS()
{
    connHandle = pbs_connect(nix::fmt("%s:%ul", ourSettings.pbsHost.get(), ourSettings.pbsPort.get()).c_str());
    if (connHandle == -1)
        throw PBSConnectionError(nix::fmt("Error connecting to PBS server: %d", pbs_errno));
}

std::string PBS::submit(nix::StorePath drvPath)
{
    auto jobNameStr = nix::fmt("Nix_Build_%s", std::string(drvPath.to_string()));
    char *jobName = jobNameStr.data();

    // We don't know the jobdir until after the job is running, so use a
    // relative path for the script generation and update it to an absolute
    // path after submission. 
    rootPath = nix::fmt("%s.root", jobName);

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

    attropl aName = {nullptr, ATTR_N, nullptr, jobName, SET};
    char kfVal[] = "oe";
    attropl aKeepFiles = {&aName, ATTR_k, nullptr, kfVal, SET};
    char pathVar[] = PATH_VAR;
    attropl aVariableList = {&aKeepFiles, ATTR_v, nullptr, pathVar, SET};

    char *id = pbs_submit(connHandle, &aVariableList, tmp_name, nullptr, nullptr);
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
    jobStderr = nix::fmt("%s/%s.e%s", jobDir, jobName, jobIdNum);
    rootPath = nix::fmt("%s/%s.root", jobDir, jobName);

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
    std::string value = serverStatus->attribs->value;
    pbs_statfree(serverStatus);
    return value;
}

int PBS::waitForJobFinish()
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(connHandle, jobId);
        if (state == "F") {
            attrl exitAttr = {nullptr, ATTR_exit_status, nullptr, nullptr, SET};
            std::this_thread::sleep_for(30s);
            batch_status *exitStatus = pbs_statjob(connHandle, jobId.data(), &exitAttr, "x");
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
    pbs_disconnect(connHandle);
}
