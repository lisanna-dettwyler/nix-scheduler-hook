#include <string>

#include <nix/store/path.hh>
#include <nix/store/build-result.hh>

#include <restclient-cpp/connection.h>
#include <restclient-cpp/restclient.h>

void slurmBuildDerivation(nix::StorePath drvPath);

std::string slurmGetJobState(RestClient::Connection *conn, std::string jobId);
