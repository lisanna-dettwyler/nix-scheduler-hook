#pragma once

#include <string>
#include <utility>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <array>

#include <nix/store/path.hh>
#include <nix/store/store-open.hh>
#include <nix/store/ssh-store.hh>
#include <nix/store/ssh.hh>
#include <nix/util/types.hh>
#include <nix/util/logging.hh>

#include "settings.hh"

class Scheduler
{
public:
    struct JobContext {
        std::string jobId;
        std::string hostname;
        std::string storeUri;
        std::string jobStderr;
        std::shared_ptr<nix::SSHMaster::Connection> cmdConn;
        std::shared_ptr<nix::SSHMaster> sshMaster;
        std::string rootPath;
        bool cmdOutInit = false;
        std::shared_ptr<__gnu_cxx::stdio_filebuf<char>> cmdOutBuf;
    };

    Scheduler() {}
    virtual ~Scheduler()
    {
        try {
            for (auto & [drvPath, jobContext] : contexts) {
                if (jobContext.sshMaster) {
                    for (auto & file : {jobContext.rootPath, jobContext.jobStderr}) {
                        nix::Strings rmCmd = {"rm", "-f", file};
                        auto cmd = jobContext.sshMaster->startCommand(std::move(rmCmd));
                        cmd->sshPid.wait();
                    }
                    if (ourSettings.collectGarbage.get()) {
                        auto binDir = ourSettings.remoteNixBinDir.get();
                        nix::Strings gcCmd = {
                            (binDir != "" ? binDir + "/" : "") + "nix-store",
                            "--gc",
                            "--store",
                            ourSettings.remoteStore.get()
                        };
                        auto cmd = jobContext.sshMaster->startCommand(std::move(gcCmd));
                        if (int rc = cmd->sshPid.wait()) {
                            using namespace nix;
                            printError("NSH Error: garbage collection failed: %d", rc);
                        }
                    }
                }
            }
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error during Scheduler teardown: %s", e.what());
        }
    }

    struct StartBuildNotCalled : public std::runtime_error
    {
        explicit StartBuildNotCalled() : std::runtime_error("startBuild() has not yet been called.") {}
    };

    /* Submits a derivation for building and establishes an ssh connection to
     * the scheduled host.
     * @return Hostname of the node assigned to the job. */
    std::string startBuild(nix::StorePath drvPath)
    {
        contexts[drvPath] = JobContext();
        auto & jobContext = contexts[drvPath];
        submit(drvPath);
        jobContext.storeUri = "ssh-ng://" + jobContext.hostname;
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", jobContext.storeUri));
        auto baseStoreConfig = nix::resolveStoreConfig(nix::StoreReference::parse(jobContext.storeUri));
        auto sshStoreConfig = std::dynamic_pointer_cast<nix::SSHStoreConfig>(baseStoreConfig.get_ptr());
        jobContext.sshMaster = std::make_shared<nix::SSHMaster>(sshStoreConfig->createSSHMaster(false));

        submitCalled.insert(drvPath);
        return jobContext.hostname;
    }

    /* Submits a derivation for building. */
    virtual void submit(nix::StorePath drvPath) = 0;

    /* Waits for the submitted job to finish.
     * @return Exit code of job, or -1 if abnormal termination (e.g. cancelled). */
    virtual int waitForJobFinish(nix::StorePath drvPath) = 0;

    std::string getJobId(nix::StorePath drvPath)
    {
        return contexts[drvPath].jobId;
    }

    std::shared_ptr<std::istream> getStderrStream(nix::StorePath drvPath)
    {
        if (!submitCalled.contains(drvPath)) throw StartBuildNotCalled();
        auto & jobContext = contexts[drvPath];
        if (!jobContext.cmdOutInit) {
            nix::Strings tailCmd = {"tail", "-f", jobContext.jobStderr};
            jobContext.cmdConn = jobContext.sshMaster->startCommand(std::move(tailCmd));
            auto cmdOutFd = jobContext.cmdConn->out.release();
            int flags = fcntl(cmdOutFd, F_GETFL, 0);
            fcntl(cmdOutFd, F_SETFL, flags | O_NONBLOCK);
            jobContext.cmdOutBuf = std::make_shared<__gnu_cxx::stdio_filebuf<char>>(cmdOutFd, std::ios::in);
            jobContext.cmdOutInit = true;
        }
        return std::make_shared<std::istream>(jobContext.cmdOutBuf.get());
    }

protected:
    std::map<nix::StorePath, JobContext> contexts;
    std::set<nix::StorePath> submitCalled;
};
