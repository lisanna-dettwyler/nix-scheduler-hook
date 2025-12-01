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

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>

static std::shared_ptr<RestClient::Connection> slurmGetConn()
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

std::pair<std::string, std::string> slurmBuildDerivation(nix::StorePath drvPath, std::string rootPath, std::string jobStderr)
{
    json req = {
        {"job", {
            // {"argv", {}},
            {"name", "Nix Build - " + std::string(drvPath.to_string())},
            {"current_working_directory", "/tmp"},
            {"environment", {"PATH=/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin"}},
            {"script", nix::fmt("#!/bin/bash\nwhile [ ! -e /nix/store/%s ]; do sleep 0.1; done; nix-store --realise /nix/store/%s --add-root %s --quiet; echo '@nsh done' >&2",
                std::string(drvPath.to_string()), std::string(drvPath.to_string()), rootPath)},
            {"standard_error", jobStderr},
        }}
    };

    if (ourSettings.slurmExtraJobSubmissionParams.get() != "") {
        json extraParams = json::parse(ourSettings.slurmExtraJobSubmissionParams.get());
        for (auto & [key, value] : extraParams.items()) {
            req["job"][key] = value;
        }
    }

    auto conn = slurmGetConn();
    RestClient::Response r = conn->post("/slurm/v0.0.43/job/submit", req.dump());
    if (r.body == "Authentication failure") {
        throw SlurmAuthenticationError(r.body);
    }
    json response = json::parse(r.body);
    if (response["errors"].size() > 0) {
        throw SlurmSubmitError(nix::fmt("%s (%d): %s",
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
        if (
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

std::string slurmGetJobState(std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        RestClient::Response qr = slurmGetConn()->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["jobs"].size() == 1) {
            return qresp["jobs"][0]["job_state"][0];
        } else {
            std::this_thread::sleep_for(sleepTime);
            sleepTime *= 2;
        }
    }
}

uint32_t slurmGetJobReturnCode(std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        RestClient::Response qr = slurmGetConn()->get("/slurm/v0.0.43/job/" + jobId);
        json qresp = json::parse(qr.body);
        if (qresp["jobs"].size() == 1 && qresp["jobs"][0]["exit_code"]["return_code"]["set"]) {
            return qresp["jobs"][0]["exit_code"]["return_code"]["number"];
        } else {
            std::this_thread::sleep_for(sleepTime);
            sleepTime *= 2;
        }
    }
}
