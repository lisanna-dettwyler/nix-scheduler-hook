#pragma once

#include <string>

#include <nix/util/configuration.hh>
#include <nix/util/types.hh>

struct Settings : public nix::Config
{
    Settings();

    nix::Path confDir;
    std::vector<nix::Path> userConfFiles;

    nix::Setting <std::string> jobScheduler {
        this,
        "slurm",
        "job-scheduler",
        "Which job scheduler to use, currently only 'slurm' is available."
    };

    nix::Setting <std::string> uid {
        this,
        "",
        "uid",
        "The UID of your account on the cluster. Tempfiles will be stored in /run/user/<uid>/nsh/."
    };

    nix::Setting <std::string> system {
        this,
        "x86_64-linux",
        "system",
        "The system type of this cluster, jobs requiring a different system will not be routed to the scheduler."
    };

    nix::Setting<nix::StringSet> systemFeatures {
        this,
        {"nsh"},
        "system-features",
        "Optional system features supported by the machines in the cluster. The default value is 'nsh'. Can be used to force derivations to build only via nix-scheduler-hook by adding 'nsh' as a required system feature."
    };

    nix::Setting<std::string> storeDir {
        this,
        "/nix/store",
        "store-dir",
        "The logical remote Nix store directory. Only change this if you know what you're doing."
    };

    nix::Setting<std::string> remoteStore {
        this,
        "auto",
        "remote-store",
        "The store URL to be used on the remote machine. Should be set to 'auto' if using the nix-daemon."
    };

    nix::Setting<std::string> slurmApiHost {
        this,
        "localhost",
        "slurm-api-host",
        "Hostname or address of the Slurm REST API endpoint."
    };

    nix::Setting<unsigned int> slurmApiPort {
        this,
        6820,
        "slurm-api-port",
        "Port to use for the Slurm REST API endpoint."
    };

    nix::Setting<std::string> slurmJwtToken {
        this,
        "",
        "slurm-jwt-token",
        "JWT token for authentication to the Slurm REST API."
    };

    nix::Setting<std::string> slurmExtraJobSubmissionParams {
        this,
        {},
        "slurm-extra-submission-params",
        "Extra parameters to set in the /job/submit API request, as a JSON dictionary that will be merged with the 'job' value."
    };
};

void loadConfFile(nix::AbstractConfig & config);

std::vector<nix::Path> getUserConfigFiles();

extern Settings ourSettings;
