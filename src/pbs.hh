#include "scheduler.hh"

#include <string>
#include <exception>

#include <nix/store/path.hh>
#include <nix/store/build-result.hh>

#include <pbs_ifl.h>

struct PBSConnectionError : public std::runtime_error
{
    explicit PBSConnectionError(const std::string &s) : std::runtime_error(s) {}
};

struct PBSSubmitError : public std::runtime_error
{
    explicit PBSSubmitError(const std::string &s) : std::runtime_error(s) {}
};

struct PBSDeletedError : public std::runtime_error
{
    explicit PBSDeletedError(const std::string &s) : std::runtime_error("Job " + s + " was unexpectedly deleted") {}
};

struct PBSQueryError : public std::runtime_error
{
    explicit PBSQueryError(const std::string &s) : std::runtime_error(s) {}
};

class PBS : public Scheduler
{
public:
    PBS();
    ~PBS();
    void submit(nix::StorePath drvPath);
    int waitForJobFinish();
protected:
    int connHandle;
    char scriptName[MAXPATHLEN + 1];
    bool createdScript = false;
};
