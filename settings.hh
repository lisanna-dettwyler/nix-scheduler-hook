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
};

void loadConfFile(nix::AbstractConfig & config);

std::vector<nix::Path> getUserConfigFiles();

extern Settings settings;
