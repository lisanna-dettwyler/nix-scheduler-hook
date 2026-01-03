#include "pbs.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <filesystem>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <thread>
using namespace std::chrono_literals;

#include <nix/util/fmt.hh>

static std::string getJobState(int conn, std::string jobId)
{
    attrl attr = {nullptr, ATTR_resv_state, nullptr, nullptr, SET};
    batch_status *status = pbs_statjob(conn, jobId.data(), &attr, nullptr);
    return status->attribs->value;
}

static void waitForJobRunning(int conn, std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(conn, jobId);
        if (state == "RESV_RUNNING")
            return;
        else if (state == "RESV_DELETED")
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
    static char tmp_template[] = "pbsscrptXXXXXX";
    char tmp_name[MAXPATHLEN + 1];
    snprintf(tmp_name, sizeof(tmp_name), "%s/%s", std::filesystem::temp_directory_path().c_str(), tmp_template);
    int fd = mkstemp(tmp_name);
    if (fd == -1)
        throw PBSSubmitError(nix::fmt("Error creating temporary file for PBS script %s", tmp_name));
    __gnu_cxx::stdio_filebuf<char> scriptOutBuf(fd, std::ios::out);
    std::ostream scriptOut(&scriptOutBuf);
    scriptOut << genScript(drvPath, rootPath);

    char *jobName = nix::fmt("Nix Build - %s", std::string(drvPath.to_string())).data();

    attropl aName = {nullptr, ATTR_N, nullptr, jobName};
    char kfVal[] = "oe";
    attropl aKeepFiles = {&aName, ATTR_k, nullptr, kfVal};
    char pathVar[] = PATH_VAR;
    attropl aVariableList = {&aKeepFiles, ATTR_v, nullptr, pathVar};

    char *id = pbs_submit(connHandle, &aVariableList, tmp_name, nullptr, nullptr);
    if (id == nullptr)
        throw PBSSubmitError(nix::fmt("Error submitting PBS job: %s", pbs_geterrmsg(connHandle)));
    jobId = id;

    waitForJobRunning(connHandle, jobId);

    unlink(tmp_name);

    attrl jobdirAttr = {nullptr, ATTR_jobdir, nullptr, nullptr, SET};
    batch_status *jobdirStatus = pbs_statjob(connHandle, jobId.data(), &jobdirAttr, nullptr);
    std::string jobDir = jobdirStatus->attribs->value;

    jobStderr = nix::fmt("%s/%s.e0", jobDir, jobName);

    attrl serverAttr = {nullptr, ATTR_server, nullptr, nullptr, SET};
    batch_status *serverStatus = pbs_statjob(connHandle, jobId.data(), &serverAttr, nullptr);
    return serverStatus->attribs->value;
}

int PBS::waitForJobFinish()
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(connHandle, jobId);
        if (state == "RESV_FINISHED") {
            attrl exitAttr = {nullptr, ATTR_exit_status, nullptr, nullptr, SET};
            batch_status *exitStatus = pbs_statjob(connHandle, jobId.data(), &exitAttr, nullptr);
            return std::atoi(exitStatus->attribs->value);
        }
        else if (state == "RESV_DELETED")
            throw PBSDeletedError(jobId);
        std::this_thread::sleep_for(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

PBS::~PBS()
{
    pbs_disconnect(connHandle);
}
