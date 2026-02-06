#include "scheduler.hh"

#include <string>
#include <exception>

#include <drmaa.h>

#include <nix/store/path.hh>

struct GridError : public std::runtime_error
{
    explicit GridError(const std::string &s) : std::runtime_error(s) {}
};

class Grid : public Scheduler
{
public:
    Grid();
    ~Grid();
    void submit(nix::StorePath drvPath);
    int waitForJobFinish();
protected:
    char scriptName[MAXPATHLEN + 1];
    bool createdScript = false;
};
