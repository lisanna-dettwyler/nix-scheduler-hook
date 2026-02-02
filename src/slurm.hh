#include "scheduler.hh"

#include <string>
#include <exception>

#include <nix/store/path.hh>
#include <nix/store/build-result.hh>

struct SlurmAuthenticationError : public std::runtime_error
{
    explicit SlurmAuthenticationError(const std::string &s) : std::runtime_error(s) {}
};

struct SlurmAPIError : public std::runtime_error
{
    explicit SlurmAPIError(const std::string &s) : std::runtime_error(s) {}
};

struct SlurmConfigError : public std::runtime_error
{
    explicit SlurmConfigError(const std::string &s) : std::runtime_error(s) {}
};

class Slurm : public Scheduler
{
public:
    Slurm();
    ~Slurm();
    void submit(nix::StorePath drvPath);
    int waitForJobFinish();
};
