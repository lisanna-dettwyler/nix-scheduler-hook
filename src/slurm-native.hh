#include "scheduler.hh"

#include <string>
#include <exception>

#include <slurm/slurm_errno.h>

#include <nix/store/path.hh>

struct SlurmNativeError : public std::runtime_error
{
    explicit SlurmNativeError(const std::string &fun) : std::runtime_error(nix::fmt("error when calling %s: %s", fun, slurm_strerror(errno))) {}
};

struct SlurmNativeConstraintError : public std::runtime_error
{
    explicit SlurmNativeConstraintError(const std::string &s) : std::runtime_error(s) {}
};

class SlurmNative : public Scheduler
{
    uint32_t nativeJobId;
public:
    SlurmNative();
    ~SlurmNative();
    void submit(nix::StorePath drvPath);
    int waitForJobFinish();
};
