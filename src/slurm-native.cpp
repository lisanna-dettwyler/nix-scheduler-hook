#include "slurm-native.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <thread>
using namespace std::chrono_literals;

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

#include <slurm/slurm.h>

SlurmNative::SlurmNative()
{
    slurm_init(ourSettings.slurmConf.get() != "" ? ourSettings.slurmConf.get().c_str() : nullptr);
}

void SlurmNative::submit(nix::StorePath drvPath)
{
    rootPath = ourSettings.slurmStateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".root";
    jobStderr = ourSettings.slurmStateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".stderr";

    job_desc_msg_t job_desc_msg;
    slurm_init_job_desc_msg(&job_desc_msg);

    char pathVar[] = PATH_VAR;
    char *vars[] = {pathVar};
    job_desc_msg.environment = vars;
    job_desc_msg.env_size = 1;

    auto script = genScript(drvPath, rootPath);
    job_desc_msg.script = script.data();

    job_desc_msg.work_dir = ourSettings.slurmStateDir.get().data();

    job_desc_msg.std_err = jobStderr.data();

    auto store = nix::openStore();
    auto drv = store->readDerivation(drvPath);
    if (drv.env.count("slurmNativeConstraints") == 1) {
        json extraParams = json::parse(drv.env["slurmNativeConstraints"]);
        for (auto & [key, value] : extraParams.items()) {
            if (key == "cpus") {
                if (value > UINT16_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.cpus_per_task = static_cast<uint16_t>(value);
            } else if (key == "memPerNode") {
                if (value > UINT64_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.pn_min_memory = static_cast<uint64_t>(value);
            } else if (key == "memPerCPU") {
                if (value > UINT64_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.pn_min_memory = static_cast<uint64_t>(value) | MEM_PER_CPU;
            } else {
                throw SlurmNativeConstraintError(nix::fmt("unknown constraint %s", key));
            }
        }
    }

    submit_response_msg_t *resp;
    if (slurm_submit_batch_job(&job_desc_msg, &resp)) {
        slurm_free_submit_response_response_msg(resp);
        throw SlurmNativeError("slurm_submit_batch_job");
    } else if (resp->error_code) {
        using namespace nix;
        printError("warning: %s", slurm_strerror(resp->error_code));
    }
    nativeJobId = resp->job_id;
    jobId = std::to_string(nativeJobId);
    slurm_free_submit_response_response_msg(resp);

    bool foundBatchHost = false;
    auto sleepTime = 50ms;
    while (!foundBatchHost) {
        job_info_msg_t *resp;
        if (slurm_load_job(&resp, nativeJobId, 0) || resp->record_count != 1) {
            slurm_free_job_info_msg(resp);
            throw SlurmNativeError("slurm_load_job");
        } else if (resp->job_array->batch_host) {
            hostname = resp->job_array->batch_host;
            slurm_free_job_info_msg(resp);
            break;
        } else {
            slurm_free_job_info_msg(resp);
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }
}

static bool isLive(job_states state)
{
    return (state == JOB_PENDING || state == JOB_RUNNING);
}

static job_states getJobState(uint32_t jobId)
{
    slurm_selected_step_t jobs = {nullptr, NO_VAL, NO_VAL, {0, jobId, 0, 0} };
    job_state_response_msg_t *resp;
    if (slurm_load_job_state(1, &jobs, &resp) || resp->jobs_count != 1) {
        slurm_free_job_state_response_msg(resp);
        throw SlurmNativeError("slurm_load_job_state");
    } else {
        job_states state = static_cast<job_states>(resp->jobs->state);
        slurm_free_job_state_response_msg(resp);
        return state;
    }
}

static uint32_t getJobReturnCode(uint32_t jobId)
{
    job_info_msg_t *resp;
    if (slurm_load_job(&resp, jobId, 0) || resp->record_count != 1) {
        slurm_free_job_info_msg(resp);
        throw SlurmNativeError("slurm_load_job");
    } else {
        uint32_t exit_code = resp->job_array->exit_code;
        slurm_free_job_info_msg(resp);
        return exit_code;
    }
}

int SlurmNative::waitForJobFinish()
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(nativeJobId);
        if (!isLive(state)) {
            if (state != JOB_COMPLETE && state != JOB_FAILED)
                return -1;
            else
                return getJobReturnCode(nativeJobId);
        } else {
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }
}

SlurmNative::~SlurmNative()
{
    if (nativeJobId && isLive(getJobState(nativeJobId))) {
        if (slurm_kill_job(nativeJobId, SIGTERM, 0) && isLive(getJobState(nativeJobId))) {
            using namespace nix;
            printError("error killing job %" PRIu32 ": %s", nativeJobId, slurm_strerror(errno));
        }
    }

    slurm_fini();
}
