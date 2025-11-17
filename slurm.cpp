#include "slurm.hh"
#include "settings.hh"

#include <ext/stdio_filebuf.h>
#include <iostream>
#include <fstream>
#include <thread>
using namespace std::chrono_literals;
#include <memory>
#include <thread>
#include <atomic>
#include <fcntl.h>

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <nix/store/ssh.hh>
#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/ssh-store.hh>
#include <nix/util/types.hh>
#include <nix/util/serialise.hh>

void slurmBuildDerivation(nix::StorePath drvPath)
{
    const std::string jobStdout = "/tmp/job-" + std::string(drvPath.to_string()) + ".stdout";
    const std::string jobStderr = "/tmp/job-" + std::string(drvPath.to_string()) + ".stderr";
    json req = {
        {"job", {
            // {"argv", {}},
            {"name", "Nix Build - " + std::string(drvPath.to_string())},
            {"current_working_directory", "/tmp"},
            {"environment", {"PATH=/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin"}},
            {"script", "#!/bin/bash\nwhile [ ! -e /nix/store/" + std::string(drvPath.to_string()) + " ]; do sleep 0.1; done; nix-store --realise /nix/store/" + std::string(drvPath.to_string())},
            {"standard_output", jobStdout},
            {"standard_error", jobStderr},
        }}
    };

    RestClient::init();
    RestClient::Connection *conn = new RestClient::Connection(nix::fmt("http://%s:%d", settings.slurmApiHost.get(), settings.slurmApiPort.get()));
    RestClient::HeaderFields headers;
    headers["X-SLURM-USER-TOKEN"] = settings.slurmJwtToken.get();
    headers["Content-Type"] = "application/json";
    conn->SetHeaders(headers);
    RestClient::Response r = conn->post("/slurm/v0.0.43/job/submit", req.dump());
    std::cout << r.body << std::endl;

    json response = json::parse(r.body);
    int jobIdInt = response["job_id"];
    std::string jobId = std::to_string(jobIdInt);
    std::cout << "got job_id " << jobId << std::endl;

    bool foundBatchHost = false;
    std::string batchHost;
    while (!foundBatchHost) {
        RestClient::Response qr = conn->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (
            qresp["jobs"].size() == 1 &&
            qresp["jobs"][0].contains("batch_host") &&
            qresp["jobs"][0]["batch_host"] != ""
        ) {
            batchHost = qresp["jobs"][0]["batch_host"];
            foundBatchHost = true;
        } else {
            std::this_thread::sleep_for(500ms);
        }
    }
    std::cout << "got batch_host [" << batchHost << "]" << std::endl;

    auto baseStoreConfig = nix::resolveStoreConfig(nix::StoreReference::parse("ssh-ng://" + batchHost));
    auto sshStoreConfig = std::dynamic_pointer_cast<nix::SSHStoreConfig>(baseStoreConfig.get_ptr());
    auto sshMaster = sshStoreConfig->createSSHMaster(false);

    std::shared_ptr<nix::Store> store = nix::openStore("ssh-ng://" + batchHost);
    store->connect();

    nix::Strings stderrTailCmd = {"tail", "-f", jobStderr};
    auto cmd = sshMaster.startCommand(std::move(stderrTailCmd));
    auto cmdOutFd = cmd->out.release();
    int flags = fcntl(cmdOutFd, F_GETFL, 0);
    fcntl(cmdOutFd, F_SETFL, flags | O_NONBLOCK);
    __gnu_cxx::stdio_filebuf<char> cmdOutBuf(cmdOutFd, std::ios::in);
    std::istream cmdOutIs(&cmdOutBuf);
    std::atomic_bool cmdDone = false;
    std::thread cmdOutThread([&]() {
        std::string line;
        while (!cmdDone) {
            if (std::getline(cmdOutIs, line)) {
                std::cerr << line << std::endl;
            } else {
                std::this_thread::sleep_for(100ms);
                cmdOutIs.clear();
            }
        }
        while (std::getline(cmdOutIs, line)) {
            std::cerr << line << std::endl;
        }
    });

    while (!cmdDone) {
        auto state = slurmGetJobState(conn, jobId);
        if (state != "PENDING" && state != "RUNNING") {
            cmdDone = true;
        }
    }

    cmdOutThread.join();
}

std::string slurmGetJobState(RestClient::Connection *conn, std::string jobId)
{
    while (true) {
        RestClient::Response qr = conn->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["jobs"].size() == 1) {
            return qresp["jobs"][0]["job_state"][0];
        } else {
            std::this_thread::sleep_for(500ms);  // TODO: progressive backoff
        }
    }
}
