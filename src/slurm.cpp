#include "slurm.hh"
#include "settings.hh"

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
using namespace std::chrono_literals;
#include <atomic>
#include <fcntl.h>
#include <array>

#include <boost/algorithm/string/join.hpp>

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

static std::shared_ptr<RestClient::Connection> getConn()
{
    static bool init = false;
    static std::shared_ptr<RestClient::Connection> conn;
    if (!init) {
        RestClient::init();
        conn = std::make_shared<RestClient::Connection>(
            nix::fmt("http://%s:%d", ourSettings.slurmApiHost.get(), ourSettings.slurmApiPort.get()));
        RestClient::HeaderFields headers;
        headers["X-SLURM-USER-TOKEN"] = ourSettings.slurmJwtToken.get();
        headers["Content-Type"] = "application/json";
        conn->SetHeaders(headers);
        init = true;
    }
    return conn;
}

static std::pair<std::string, std::string> buildDerivation(nix::StorePath drvPath, std::string rootPath, std::string jobStderr)
{
    json req = {
        {"job", {
            // {"argv", {}},
            {"name", "Nix Build - " + std::string(drvPath.to_string())},
            {"current_working_directory", "/tmp"},
            {"environment", {"PATH=/run/current-system/sw/bin/:/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin"}},
            {"script", nix::fmt(
                "#!/bin/sh\n"
                "while ! nix-store --store '%s' --query --hash %s/%s >/dev/null 2>&1; do sleep 0.1; done;"
                "nix-store --store '%s' --realise %s/%s --option system-features '%s' --add-root %s --quiet;"
                "rc=$?;"
                "echo '@nsh done' >&2;"
                "exit $rc",
                ourSettings.remoteStore.get(),
                ourSettings.storeDir.get(), std::string(drvPath.to_string()),
                ourSettings.remoteStore.get(),
                ourSettings.storeDir.get(), std::string(drvPath.to_string()),
                boost::algorithm::join(ourSettings.systemFeatures.get(), " "),
                rootPath
            )},
            {"standard_error", jobStderr},
        }}
    };

    auto store = nix::openStore();
    auto drv = store->readDerivation(drvPath);
    if (drv.env.count("extraSlurmParams") == 1) {
        json extraParams = json::parse(drv.env["extraSlurmParams"]);
        for (auto & [key, value] : extraParams.items()) {
            req["job"][key] = value;
        }
    }

    if (ourSettings.slurmExtraJobSubmissionParams.get() != "") {
        json extraParams = json::parse(ourSettings.slurmExtraJobSubmissionParams.get());
        for (auto & [key, value] : extraParams.items()) {
            req["job"][key] = value;
        }
    }

    auto conn = getConn();
    RestClient::Response r = conn->post("/slurm/v0.0.43/job/submit", req.dump());
    if (r.body == "Authentication failure") {
        throw SlurmAuthenticationError(r.body);
    }
    json response = json::parse(r.body);
    if (response["errors"].size() > 0) {
        throw SlurmAPIError(nix::fmt("%s (%d): %s",
            response["errors"][0]["description"],
            response["errors"][0]["error_number"],
            response["errors"][0]["error"]));
    }
    int jobIdInt = response["job_id"];
    std::string jobId = std::to_string(jobIdInt);

    bool foundBatchHost = false;
    std::string batchHost;
    auto sleepTime = 50ms;
    while (!foundBatchHost) {
        RestClient::Response qr = conn->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                qresp["errors"][0]["description"],
                qresp["errors"][0]["error_number"],
                qresp["errors"][0]["error"]));
        } else if (
            qresp["jobs"].size() == 1 &&
            qresp["jobs"][0].contains("batch_host") &&
            qresp["jobs"][0]["batch_host"] != ""
        ) {
            batchHost = qresp["jobs"][0]["batch_host"];
            foundBatchHost = true;
        } else {
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }

    return {batchHost, jobId};
}

static bool isLive(std::string state)
{
    return (state == "PENDING" || state == "RUNNING");
}

static std::string getJobState(std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        RestClient::Response qr = getConn()->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                qresp["errors"][0]["description"],
                qresp["errors"][0]["error_number"],
                qresp["errors"][0]["error"]));
        } else if (qresp["jobs"].size() == 1) {
            return qresp["jobs"][0]["job_state"][0];
        } else {
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 2s) sleepTime *= 2;
        }
    }
}

static uint32_t getJobReturnCode(std::string jobId)
{
    while (true) {
        RestClient::Response qr = getConn()->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                qresp["errors"][0]["description"],
                qresp["errors"][0]["error_number"],
                qresp["errors"][0]["error"]));
        } else if (qresp["jobs"].size() == 1 && qresp["jobs"][0]["exit_code"]["return_code"]["set"]) {
            return qresp["jobs"][0]["exit_code"]["return_code"]["number"];
        } else {
            std::this_thread::sleep_for(50ms);
        }
    }
}

std::string Slurm::submit(nix::StorePath drvPath)
{
    auto r = buildDerivation(drvPath, rootPath, jobStderr);
    hostname = r.first;
    jobId = r.second;

    return hostname;
}

int Slurm::waitForJobFinish()
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(jobId);
        if (!isLive(state)) {
            if (state != "COMPLETED" && state != "FAILED")
                return -1;
            else
                return getJobReturnCode(jobId);
        } else {
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 4s) sleepTime *= 2;
        }
    }
}

Slurm::~Slurm()
{
    if (isLive(getJobState(jobId))) {
        getConn()->del("/slurm/v0.0.43/job/" + jobId);
    }

    for (auto & file : std::array<std::string, 2>{rootPath, jobStderr}) {
        nix::Strings rmCmd = {"bash", "-c", "rm -fv " + file + "; echo done"};
        auto cmd = sshMaster->startCommand(std::move(rmCmd));
        auto cmdBuf = __gnu_cxx::stdio_filebuf<char>(cmd->out.release(), std::ios::in);
        auto cmdStream = std::istream(&cmdBuf);
        cmdStream.get();
    }
}
