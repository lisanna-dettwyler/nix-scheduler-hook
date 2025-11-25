#include <iostream>
#include <optional>
#include <thread>
#include <memory>
using namespace std::chrono_literals;
#include <ext/stdio_filebuf.h>

// #include <restc-cpp/restc-cpp.h>
// #include <restc-cpp/RequestBuilder.h>
// using namespace restc_cpp;

#include <nix/main/shared.hh>
#include <nix/main/plugin.hh>
#include <nix/util/fmt.hh>
#include <nix/store/path.hh>
#include <nix/store/store-open.hh>
#include <nix/store/build-result.hh>
#include <nix/store/ssh-store.hh>
#include <nix/store/globals.hh>
#include <nix/store/pathlocks.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/store/ssh.hh>
#include <nix/store/local-store.hh>
#include <nix/util/types.hh>
#include <nix/util/serialise.hh>
#include <nix/util/logging.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/error.hh>
#include <nix/util/util.hh>
#include <nix/util/hash.hh>

#include "settings.hh"
#include "slurm.hh"
#include "logging.hh"

static void handleAlarm(int sig) {}

static std::string currentLoad;

static std::string escapeUri(std::string uri)
{
    std::replace(uri.begin(), uri.end(), '/', '_');
    return uri;
}

int main(int argc, char **argv)
{
    nix::logger = nix::makeJSONLogger(nix::getStandardError());

    /* Ensure we don't get any SSH passphrase or host key popups. */
    unsetenv("DISPLAY");
    unsetenv("SSH_ASKPASS");

    if (argc != 2)
        throw nix::UsageError("called without required arguments");

    nix::verbosity = (nix::Verbosity) std::stoll(argv[1]);

    nix::FdSource source(STDIN_FILENO);

    /* Read the parent's settings. */
    while (nix::readInt(source)) {
        auto name = nix::readString(source);
        auto value = nix::readString(source);
        nix::settings.set(name, value);
    }

    try {
        auto s = nix::readString(source);
        if (s != "try")
            return 0;
    } catch (nix::EndOfFile &) {
        return 0;
    }

    nix::initNix();
    nix::initPlugins();
    auto store = nix::openStore();

    /* It would be more appropriate to use $XDG_RUNTIME_DIR, since
        that gets cleared on reboot, but it wouldn't work on macOS. */
    auto currentLoadName = "/current-load";
    if (auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>())
        currentLoad = std::string{localStore->config.stateDir} + currentLoadName;
    else
        currentLoad = nix::settings.nixStateDir + currentLoadName;

    // amWilling (unused)
    nix::readInt(source);

    auto neededSystem = nix::readString(source);
    if (neededSystem != ourSettings.system.get()) {
        std::cerr << "# decline\n";
        return 0;
    }

    nix::StorePath drvPath = store->parseStorePath(readString(source));

    // If special features are required, assume the cluster does not have them
    // TODO: add a setting to specify supportedFeatures and mandatoryFeatures
    auto requiredFeatures = nix::readStrings<nix::StringSet>(source);
    if (!requiredFeatures.empty()) {
        std::cerr << "# decline\n";
        return 0;
    }
    
    // nix::BuildResult optResult;
    
    const std::string jobStdout = "/tmp/job-" + std::string(drvPath.to_string()) + ".stdout";
    const std::string jobStderr = "/tmp/job-" + std::string(drvPath.to_string()) + ".stderr";
    
    ::loadConfFile(ourSettings);
    
    std::string host;
    std::string jobId;
    if (ourSettings.jobScheduler.get() == "slurm") {
        try {
            auto r = slurmBuildDerivation(drvPath, jobStdout, jobStderr);
            host = r.first;
            jobId = r.second;
        } catch (std::exception & e) {
            using namespace nix;
            printError("Error when attempting to build derivation on Slurm: %s", e.what());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    } else {
        using namespace nix;
        printError("unsupported job scheduler %s", ourSettings.jobScheduler.get());
        std::cerr << "# decline-permanently\n";
        return 0;
    }

    const std::string storeUri = "ssh-ng://" + host;
    
    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", storeUri));
    }

    auto baseStoreConfig = nix::resolveStoreConfig(nix::StoreReference::parse(storeUri));
    auto sshStoreConfig = std::dynamic_pointer_cast<nix::SSHStoreConfig>(baseStoreConfig.get_ptr());
    auto sshMaster = sshStoreConfig->createSSHMaster(false);
    
    std::shared_ptr<nix::Store> sshStore;
    try {
        sshStore = nix::openStore(storeUri);
        sshStore->connect();
    } catch (std::exception & e) {
        auto msg = nix::chomp(nix::drainFD(5, false));
        using namespace nix;
        printError("cannot build on '%s': %s%s", storeUri, e.what(), msg.empty() ? "" : ": " + msg);
        std::cerr << "# decline\n";
        return 0;
    }

    std::cerr << "# accept\n" << storeUri << "\n";

    auto inputs = nix::readStrings<nix::PathSet>(source);
    auto wantedOutputs = nix::readStrings<nix::StringSet>(source);

    nix::AutoCloseFD uploadLock;
    {
        auto setUpdateLock = [&](auto && fileName) {
            uploadLock = nix::openLockFile(currentLoad + "/" + escapeUri(fileName) + ".upload-lock", true);
        };
        try {
            setUpdateLock(storeUri);
        } catch (nix::SysError & e) {
            if (e.errNo != ENAMETOOLONG)
                throw;
            // Try again hashing the store URL so we have a shorter path
            auto h = nix::hashString(nix::HashAlgorithm::MD5, storeUri);
            setUpdateLock(h.to_string(nix::HashFormat::Base64, false));
        }
    }

    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("waiting for the upload lock to '%s'", storeUri));

        auto old = signal(SIGALRM, handleAlarm);
        alarm(15 * 60);
        if (!nix::lockFile(uploadLock.get(), nix::LockType::ltWrite, true)) {
            using namespace nix;
            printError("somebody is hogging the upload lock for '%s', continuing...");
        }
        alarm(0);
        signal(SIGALRM, old);
    }

    auto substitute = nix::settings.buildersUseSubstitutes ? nix::Substitute : nix::NoSubstitute;

    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("copying dependencies to '%s'", storeUri));
        nix::copyPaths(*store, *sshStore, store->parseStorePathSet(inputs), nix::NoRepair, nix::NoCheckSigs, substitute);
        nix::PathSet rootDrv;
        rootDrv.insert(store->printStorePath(drvPath));
        nix::copyPaths(*store, *sshStore, store->parseStorePathSet(rootDrv), nix::NoRepair, nix::NoCheckSigs, substitute);
    }

    uploadLock = -1;

    nix::Strings stderrTailCmd = {"tail", "-f", jobStderr};
    auto cmd = sshMaster.startCommand(std::move(stderrTailCmd));
    auto cmdOutFd = cmd->out.release();
    int flags = fcntl(cmdOutFd, F_GETFL, 0);
    fcntl(cmdOutFd, F_SETFL, flags | O_NONBLOCK);
    __gnu_cxx::stdio_filebuf<char> cmdOutBuf(cmdOutFd, std::ios::in);
    std::istream cmdOutIs(&cmdOutBuf);
    std::atomic_bool cmdAbend = false;
    std::thread cmdOutThread([&]() {
        using namespace nix;
        bool gotTerminator = false;
        while (!cmdAbend && !gotTerminator) {
            std::string data;
            char c;
            while (cmdOutIs.get(c)) {
                data += c;
            }
            if (data != "") {
                gotTerminator = handleOutput(data);
            } else {
                std::this_thread::sleep_for(100ms);
                cmdOutIs.clear();
            }
        }
        std::string data;
        char c;
        while (cmdOutIs.get(c)) {
            data += c;
        }
        if (data != "") {
            handleOutput(data);
        }
    });

    bool cmdDone = false;
    while (!cmdDone) {
        if (ourSettings.jobScheduler.get() == "slurm") {
            auto state = slurmGetJobState(jobId);
            if (state != "PENDING" && state != "RUNNING") {
                cmdDone = true;
                if (state != "COMPLETED")
                    cmdAbend = true;
            }
        } else {
            using namespace nix;
            printError("unsupported job scheduler %s", ourSettings.jobScheduler.get());
            return 1;
        }
    }

    cmdOutThread.join();

    uint32_t code = slurmGetJobReturnCode(jobId);
    if (code) {
        // Build failed, so no more work to do
        return code;
    }
    if (cmdAbend) {
        return 1;
    }

    using namespace nix;
    auto drv = store->readDerivation(drvPath);
    auto outputHashes = staticOutputHashes(*store, drv);
    std::set<Realisation> missingRealisations;
    StorePathSet missingPaths;
    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations) && !drv.type().hasKnownOutputPaths()) {
        for (auto & outputName : wantedOutputs) {
            auto thisOutputHash = outputHashes.at(outputName);
            auto thisOutputId = DrvOutput{thisOutputHash, outputName};
            auto r = store->queryRealisation(thisOutputId);
            if (!r) {
                debug("missing output %s", outputName);
                missingRealisations.insert(*r);
                missingPaths.insert(r->outPath);
            }
        }
    } else {
        auto outputPaths = drv.outputsAndOptPaths(*store);
        for (auto & [outputName, hopefullyOutputPath] : outputPaths) {
            assert(hopefullyOutputPath.second);
            if (!store->isValidPath(*hopefullyOutputPath.second))
                missingPaths.insert(*hopefullyOutputPath.second);
        }
    }

    if (!missingPaths.empty()) {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("copying outputs from '%s'", storeUri));
        if (auto localStore = store.dynamic_pointer_cast<LocalStore>())
            for (auto & path : missingPaths)
                localStore->locksHeld.insert(store->printStorePath(path)); /* FIXME: ugly */
        copyPaths(*sshStore, *store, missingPaths, NoRepair, NoCheckSigs, NoSubstitute);
    }
    // XXX: Should be done as part of `copyPaths`
    for (auto & realisation : missingRealisations) {
        // Should hold, because if the feature isn't enabled the set
        // of missing realisations should be empty
        experimentalFeatureSettings.require(Xp::CaDerivations);
        store->registerDrvOutput(realisation);
    }

    return 0;
}
