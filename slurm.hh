#include "scheduler.hh"

#include <string>
#include <exception>

#include <nix/store/path.hh>
#include <nix/store/build-result.hh>

#include <restclient-cpp/connection.h>
#include <restclient-cpp/restclient.h>

struct SlurmAuthenticationError : public std::runtime_error
{
    explicit SlurmAuthenticationError(const std::string &s) : std::runtime_error(s) {}
};

struct SlurmSubmitError : public std::runtime_error
{
    explicit SlurmSubmitError(const std::string &s) : std::runtime_error(s) {}
};

class Slurm : public Scheduler
{
public:
    Slurm() {}
    ~Slurm();
    std::string submit(nix::StorePath drvPath);
    int waitForJobFinish();
};
