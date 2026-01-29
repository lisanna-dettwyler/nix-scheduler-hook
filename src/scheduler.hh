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
    Scheduler() {}
    virtual ~Scheduler()
    {
        if (sshMaster) {
            for (auto & file : std::array<std::string, 2>{rootPath, jobStderr}) {
                nix::Strings rmCmd = {"bash", "-c", "rm -fv " + file + "; echo done"};
                auto cmd = sshMaster->startCommand(std::move(rmCmd));
                auto cmdBuf = __gnu_cxx::stdio_filebuf<char>(cmd->out.release(), std::ios::in);
                auto cmdStream = std::istream(&cmdBuf);
                cmdStream.get();
            }
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
        rootPath = ourSettings.stateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".root";
        jobStderr = ourSettings.stateDir.get() + "/job-" + std::string(drvPath.to_string()) + ".stderr";

        hostname = submit(drvPath);
        storeUri = "ssh-ng://" + hostname;
        {
            nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", storeUri));
        }
        auto baseStoreConfig = nix::resolveStoreConfig(nix::StoreReference::parse(storeUri));
        auto sshStoreConfig = std::dynamic_pointer_cast<nix::SSHStoreConfig>(baseStoreConfig.get_ptr());
        // nix::SSHMaster does not permit assignment
        static auto ssh = sshStoreConfig->createSSHMaster(false);
        sshMaster = &ssh;

        submitCalled = true;
        return hostname;
    }

    /* Submits a derivation for building.
     * @return Hostname of the node assigned to the job. */
    virtual std::string submit(nix::StorePath drvPath) = 0;

    /* Waits for the submitted job to finish.
     * @return Exit code of job, or -1 if abnormal termination (e.g. cancelled). */
    virtual int waitForJobFinish() = 0;

    std::string getJobId()
    {
        return jobId;
    }

    std::shared_ptr<std::istream> getStderrStream()
    {
        if (!submitCalled) throw StartBuildNotCalled();
        if (!cmdOutInit) {
            nix::Strings tailCmd = {"tail", "-f", jobStderr};
            cmdConn = sshMaster->startCommand(std::move(tailCmd));
            auto cmdOutFd = cmdConn->out.release();
            int flags = fcntl(cmdOutFd, F_GETFL, 0);
            fcntl(cmdOutFd, F_SETFL, flags | O_NONBLOCK);
            cmdOutBuf = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(cmdOutFd, std::ios::in);
            cmdOutInit = true;
        }
        return std::make_shared<std::istream>(cmdOutBuf.get());
    }

protected:
    std::string jobId;
    std::string hostname;
    std::string storeUri;
    std::string jobStderr;
    std::unique_ptr<nix::SSHMaster::Connection> cmdConn;
    nix::SSHMaster *sshMaster;
    std::string rootPath;
    std::atomic<bool> cmdOutInit = false;
    std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> cmdOutBuf;

    std::atomic<bool> submitCalled = false;
};
