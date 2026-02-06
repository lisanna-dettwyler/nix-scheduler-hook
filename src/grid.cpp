#include "grid.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <thread>
using namespace std::chrono_literals;

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

Grid::Grid()
{
    int errnum;
    char error[DRMAA_ERROR_STRING_BUFFER];
    errnum = drmaa_init(nullptr, error, DRMAA_ERROR_STRING_BUFFER);
    if (errno)
        throw GridError(nix::fmt("error in drmaa_init: %s", std::string(error)));
}

void Grid::submit(nix::StorePath drvPath)
{
    auto jobName = nix::fmt("Nix_Build_%s", std::string(drvPath.to_string()));

    rootPath = ourSettings.gridStateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".root";
    jobStderr = ourSettings.gridStateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".stderr";

    char tmp_template[] = "gridscrptXXXXXX";
    snprintf(scriptName, sizeof(scriptName), "%s/%s", std::filesystem::temp_directory_path().c_str(), tmp_template);
    int fd = mkstemp(scriptName);
    if (fd == -1)
        throw GridError(nix::fmt("Error creating temporary file for Grid script %s", scriptName));
    createdScript = true;
    std::filesystem::permissions(scriptName, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    __gnu_cxx::stdio_filebuf<char> scriptOutBuf(fd, std::ios::out);
    std::ostream scriptOut(&scriptOutBuf);
    scriptOut << genScript(drvPath, rootPath);
    scriptOut.flush();

    int errnum;
    char error[DRMAA_ERROR_STRING_BUFFER];
    drmaa_job_template_t *jt = nullptr;
    errnum = drmaa_allocate_job_template(&jt, error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_allocate_job_template: %s", std::string(error)));

    errnum = drmaa_set_attribute(jt, DRMAA_REMOTE_COMMAND, scriptName.c_str(), error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_set_attribute for %s: %s", DRMAA_REMOTE_COMMAND, error));

    errnum = drmaa_set_attribute(jt, DRMAA_JOB_NAME, jobName.c_str(), error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_set_attribute for %s: %s", DRMAA_JOB_NAME, error));

    errnum = drmaa_set_attribute(jt, DRMAA_ERROR_PATH, jobStderr.c_str(), error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_set_attribute for %s: %s", DRMAA_ERROR_PATH, error));

    char jobid[DRMAA_JOBNAME_BUFFER];
    errnum = drmaa_run_job(jobid, DRMAA_JOBNAME_BUFFER, jt, error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_run_job: %s", std::string(error)));

    auto sleepTime = 50ms;
    while (true) {
    }
}

static bool isLive(int state)
{
    return (
      state == DRMAA_PS_QUEUED_ACTIVE ||
      state == DRMAA_PS_SYSTEM_ON_HOLD ||
      state == DRMAA_PS_USER_ON_HOLD ||
      state == DRMAA_PS_USER_SYSTEM_ON_HOLD ||
      state == DRMAA_PS_RUNNING
    );
}

static int getJobState(std::string jobId)
{
    int errnum;
    char error[DRMAA_ERROR_STRING_BUFFER];
    int status;
    errnum = drmaa_job_ps(jobId.c_str(), &status, error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_job_ps: %s", std::string(error)));

    return status;
}

int Grid::waitForJobFinish()
{
    char error[DRMAA_ERROR_STRING_BUFFER];
    int errnum;
    int status;
    drmaa_attr_values_t *rusage = nullptr;
    errnum = drmaa_wait(jobId.c_str(), nullptr, DRMAA_JOBNAME_BUFFER, &status, DRMAA_TIMEOUT_WAIT_FOREVER, &rusage, error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum)
        throw GridError(nix::fmt("error in drmaa_wait: %s", std::string(error)));

    int aborted = 0;
    drmaa_wifaborted(&aborted, status, nullptr, 0);
    if (aborted) {
        using namespace nix;
        printError("WARNING: job %s aborted!", jobId);
        return 1;
    }

    int exitStatusAvailable = 0;
    drmaa_wifexited(&exitStatusAvailable, status, nullptr, 0);
    if (!exitStatusAvailable)
        throw GridError(nix::fmt("Job %s exited without an exit status being reported", jobId));
    int exitStatus;
    drmaa_wexitstatus(&exitStatus, status, nullptr, 0);
    return exitStatus;
}

Grid::~Grid()
{
    int errnum;
    char error[DRMAA_ERROR_STRING_BUFFER];

    if (isLive(getJobState(jobId))) {
        errnum = drmaa_control(jobId.c_str(), DRMAA_CONTROL_TERMINATE, error, DRMAA_ERROR_STRING_BUFFER);
        if (errnum && isLive(getJobState(jobId))) {
            using namespace nix;
            printError(nix::fmt("error terminating job %s: %s", jobId, std::string(error)));
        }
    }

    errnum = drmaa_exit(error, DRMAA_ERROR_STRING_BUFFER);
    if (errnum) {
        using namespace nix;
        printError(nix::fmt("error in drmaa_exit: %s", std::string(error)));
    }
}
