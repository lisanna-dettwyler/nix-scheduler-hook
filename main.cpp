#include <iostream>
#include <optional>

// #include <restc-cpp/restc-cpp.h>
// #include <restc-cpp/RequestBuilder.h>
// using namespace restc_cpp;

#include <nix/main/shared.hh>
#include <nix/main/plugin.hh>
#include <nix/util/fmt.hh>
#include <nix/store/path.hh>
#include <nix/store/store-open.hh>
#include <nix/store/build-result.hh>
#include <nix/util/logging.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/error.hh>

#include "settings.hh"
#include "slurm.hh"

// using namespace nsh;

int main(int argc, char **argv)
{
    nix::logger = nix::makeJSONLogger(nix::getStandardError());

    /* Ensure we don't get any SSH passphrase or host key popups. */
    unsetenv("DISPLAY");
    unsetenv("SSH_ASKPASS");

    if (argc != 2)
        throw nix::UsageError("called without required arguments");

    loadConfFile(settings);

    // nix::BuildResult optResult;

    nix::initNix();
    nix::initPlugins();
    auto store = nix::openStore();
    nix::StorePath drvPath = store->parseStorePath(std::string(argv[1]));

    if (settings.jobScheduler.get() == "slurm") {
        std::cout << "slurm selected as job scheduler" << std::endl;
        std::cout << "host = " << settings.slurmApiHost.get() << std::endl;
        slurmBuildDerivation(drvPath);
    }
}
