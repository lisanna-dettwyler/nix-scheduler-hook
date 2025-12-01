#include <string>
#include <exception>

#include <nix/store/path.hh>
#include <nix/store/build-result.hh>

#include <restclient-cpp/connection.h>
#include <restclient-cpp/restclient.h>

class SlurmAuthenticationError : public std::runtime_error
{
public:
    explicit SlurmAuthenticationError(const std::string &s) :
        std::runtime_error(s) {}
};

class SlurmSubmitError : public std::runtime_error
{
public:
    explicit SlurmSubmitError(const std::string &s) :
        std::runtime_error(s) {}
};

std::pair<std::string, std::string> slurmBuildDerivation(nix::StorePath drvPath, std::string rootPath, std::string jobStderr);

std::string slurmGetJobState(std::string jobId);

uint32_t slurmGetJobReturnCode(std::string jobId);
