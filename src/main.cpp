#include <iostream>
#include <optional>
#include <thread>
using namespace std::chrono_literals;
#include <memory>
#include <ext/stdio_filebuf.h>

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
#include <nix/util/signals-impl.hh>
#include <nix/util/processes.hh>
#include <nix/util/environment-variables.hh>
#include <nix/util/config-global.hh>

#include "settings.hh"
#include "slurm.hh"
#include "pbs.hh"
#include "logging.hh"

static void handleAlarm(int sig) {}

static std::string currentLoad;

static std::string escapeUri(std::string uri)
{
    std::replace(uri.begin(), uri.end(), '/', '_');
    return uri;
}

struct SigHandlerExit : public std::exception
{
    explicit SigHandlerExit() : std::exception() {}
};

static void sigHandler(int signo)
{
    throw SigHandlerExit();
}

std::filesystem::path getBuildRemoteFromNixBin(std::filesystem::path nixBin)
{
    if (std::filesystem::is_symlink(nixBin))
        nixBin = std::filesystem::read_symlink(nixBin);
    return nixBin.parent_path().parent_path() / "libexec" / "nix" / "build-remote";
}

struct FallbackHookInstance
{
    FallbackHookInstance(
      int amWilling,
      std::string neededSystem,
      std::string drvPath,
      nix::StringSet requiredFeatures,
      nix::FdSource & source
    ) {
        toHook.create();

        pid = nix::startProcess([&]() {
            if (dup2(toHook.readSide.get(), STDIN_FILENO) == -1)
                throw nix::SysError("redirecting NSH's toHook to build-remote's STDIN");

            std::filesystem::path nixBinPath = "nix";
            auto nixBinDirOpt = nix::getEnvNonEmpty("NIX_BIN_DIR");
            if (nixBinDirOpt)
                nixBinPath = std::filesystem::path(*nixBinDirOpt) / "nix";

            nix::Strings args{nixBinPath.filename().string(), "__build-remote", std::to_string(nix::verbosity)};
            execvp(nixBinPath.native().c_str(), nix::stringsToCharPtrs(args).data());

            // If nix __build-remote doesn't work, try the legacy libexec/nix/build-remote symlink
            std::filesystem::path buildRemotePath;
            if (nixBinDirOpt) {
                std::filesystem::path nixBin = std::filesystem::path(*nixBinDirOpt) / "nix";
                if (std::filesystem::exists(nixBin))
                    buildRemotePath = getBuildRemoteFromNixBin(nixBin);
            }
            else if (auto pathOpt = nix::getEnvNonEmpty("PATH")) {
                auto paths = nix::tokenizeString<nix::Strings>(*pathOpt, ":");
                for (auto path : paths) {
                    std::filesystem::path nixBin = std::filesystem::path(path) / "nix";
                    if (std::filesystem::exists(nixBin)) {
                        buildRemotePath = getBuildRemoteFromNixBin(nixBin);
                        break;
                    }
                }
            }
            if (!buildRemotePath.empty()) {
                nix::Strings args2{buildRemotePath.filename().string(), std::to_string(nix::verbosity)};
                execv(buildRemotePath.native().c_str(), nix::stringsToCharPtrs(args2).data());
            }

            throw nix::SysError("executing normal build hook");
        });

        toHook.readSide = -1;

        sink = nix::FdSink(toHook.writeSide.get());
        std::map<std::string, nix::Config::SettingInfo> settings;
        nix::globalConfig.getSettings(settings);
        for (auto & setting : settings)
            sink << 1 << setting.first << setting.second.value;
        sink << 0;

        sink << "try" << amWilling << neededSystem << drvPath << requiredFeatures;
        sink.flush();

        auto inputs = nix::readStrings<nix::PathSet>(source);
        auto wantedOutputs = nix::readStrings<nix::StringSet>(source);

        sink << inputs << wantedOutputs;
        sink.flush();
    }

    int wait()
    {
        return pid.wait();
    }

    ~FallbackHookInstance()
    {
        if (pid != -1) {
            pid.kill();
            pid.wait();
        }
    }

    nix::Pipe toHook;
    nix::Pid pid;
    nix::FdSink sink;
};

int main(int argc, char **argv)
{
try {
    /* Ensure destructors are called if terminated by Nix */
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigHandler;
    if (sigaction(SIGTERM, &act, 0))
        throw nix::SysError("assigning handler for SIGTERM");

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

    nix::initLibStore();
    nix::initPlugins();
    auto store = nix::openStore();

    /* It would be more appropriate to use $XDG_RUNTIME_DIR, since
        that gets cleared on reboot, but it wouldn't work on macOS. */
    auto currentLoadName = "/current-load";
    if (auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>())
        currentLoad = std::string{localStore->config.stateDir} + currentLoadName;
    else
        currentLoad = nix::settings.nixStateDir + currentLoadName;

    int amWilling = nix::readInt(source);

    ::loadConfFile(ourSettings);

    auto neededSystem = nix::readString(source);
    nix::StorePath drvPath = store->parseStorePath(nix::readString(source));
    auto requiredFeatures = nix::readStrings<nix::StringSet>(source);

    bool tryFallback = false;

    if (neededSystem != ourSettings.system.get()) {
        using namespace nix;
        printError("needed system %s does not match our system %s", neededSystem, ourSettings.system.get());
        tryFallback = true;
    }

    auto systemFeatures = ourSettings.systemFeatures.get();
    for (auto & feature : requiredFeatures) {
        if (systemFeatures.find(feature) == systemFeatures.end()) {
            using namespace nix;
            printError("required feature %s not available, available features:", feature);
            for (auto & f : systemFeatures) {
                printError(f);
            }
            tryFallback = true;
        }
    }

    if (tryFallback) {
        try {
            nix::Activity act(*nix::logger, nix::lvlInfo, nix::actUnknown, "falling back to normal build hook");
            return FallbackHookInstance(amWilling, neededSystem, store->printStorePath(drvPath), requiredFeatures, source).wait();
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: unable to fallback to normal build hook: %s", e.what());
            std::cerr << "# decline\n";
            return 0;
        }
    }

    std::unique_ptr<Scheduler> scheduler;
    try {
        if (ourSettings.jobScheduler.get() == "slurm") {
            scheduler = std::make_unique<Slurm>();
        } else if (ourSettings.jobScheduler.get() == "pbs") {
            scheduler = std::make_unique<PBS>();
        } else {
            using namespace nix;
            printError("NSH Error: unsupported job scheduler %s", ourSettings.jobScheduler.get());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    } catch (std::exception & e) {
        using namespace nix;
        printError("NSH Error: %s", e.what());
        std::cerr << "# decline-permanently\n";
        return 0;
    }

    std::string host;
    try {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, "submitting build to scheduler");
        host = scheduler->startBuild(drvPath);
    } catch (std::exception & e) {
        using namespace nix;
        printError("NSH Error: error when attempting to build derivation on %s: %s", ourSettings.jobScheduler.get(), e.what());
        std::cerr << "# decline-permanently\n";
        return 0;
    }
    nix::Activity startedJobAct(*nix::logger, nix::lvlInfo, nix::actUnknown, nix::fmt("started job %s on %s", scheduler->getJobId(), host));

    const std::string storeUri = "ssh-ng://" + host;
    std::shared_ptr<nix::Store> sshStore;
    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", storeUri));
        try {
            nix::StoreReference::Params params = {{"remote-store", ourSettings.remoteStore.get()}};
            sshStore = nix::openStore(storeUri, params);
            sshStore->connect();
        } catch (std::exception & e) {
            auto msg = nix::chomp(nix::drainFD(5, false));
            using namespace nix;
            printError("NSH Error: cannot build on '%s': %s%s", storeUri, e.what(), msg.empty() ? "" : ": " + msg);
            std::cerr << "# decline\n";
            return 0;
        }
    }

    std::cerr << "# accept\n" << storeUri << "\n";

    auto inputs = nix::readStrings<nix::PathSet>(source);
    auto wantedOutputs = nix::readStrings<nix::StringSet>(source);

    mkdir(currentLoad.c_str(), 0777);

    nix::AutoCloseFD uploadLock;
    {
        auto setUpdateLock = [&](auto && fileName) {
            uploadLock = nix::openLockFile(currentLoad + "/" + escapeUri(fileName) + ".upload-lock", true);
        };
        try {
            setUpdateLock(storeUri);
        } catch (nix::SysError & e) {
            if (e.errNo != ENAMETOOLONG) {
                using namespace nix;
                printError(e.what());
                throw;
            }
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
            printError("NSH Error: somebody is hogging the upload lock for '%s', continuing...");
        }
        alarm(0);
        signal(SIGALRM, old);
    }

    auto substitute = nix::settings.buildersUseSubstitutes ? nix::Substitute : nix::NoSubstitute;

    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("copying dependencies to '%s'", storeUri));
        try {
            nix::copyPaths(*store, *sshStore, store->parseStorePathSet(inputs), nix::NoRepair, nix::NoCheckSigs, substitute);
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error when attempting to copy build dependencies: %s", e.what());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
        nix::PathSet rootDrv;
        rootDrv.insert(store->printStorePath(drvPath));
        try {
            nix::copyClosure(*store, *sshStore, store->parseStorePathSet(rootDrv), nix::NoRepair, nix::NoCheckSigs, substitute);
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error when attempting to copy root derivation closure: %s", e.what());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    }

    uploadLock = -1;

    std::atomic<bool> cmdAbend = false;

    std::thread cmdOutThread([&]() {
        auto cmdOutIs = scheduler->getStderrStream();

        // The invoking Nix process listens on fd 4 for the build log
        // See https://github.com/NixOS/nix/blob/master/src/libstore/unix/build/hook-instance.cc#L61
        __gnu_cxx::stdio_filebuf<char> logBuf(4, std::ios::out);
        std::ostream logOs(&logBuf);

        bool gotTerminator = false;
        while (!gotTerminator && !cmdAbend) {
            std::string data;
            char c;
            while (cmdOutIs->get(c)) {
                data += c;
            }
            if (data != "") {
                gotTerminator = handleOutput(logOs, data);
            } else {
                std::this_thread::yield();
                cmdOutIs->clear();
            }
        }
        if (cmdAbend) {
            // Drain in the case of abnormal termination
            std::string data;
            char c;
            while (cmdOutIs->get(c)) {
                data += c;
            }
            if (data != "") {
                handleOutput(logOs, data);
            }
        }
    });

    int rc;
    try {
        rc = scheduler->waitForJobFinish();
    } catch (std::exception & e) {
        using namespace nix;
        printError("NSH Error: error while waiting for job %s termination: %s", scheduler->getJobId(), e.what());
        cmdAbend = true;
        cmdOutThread.join();
        return 1;
    }
    if (rc == -1) {
        using namespace nix;
        printError("NSH Error: job %s abnormally terminated.", scheduler->getJobId());
        cmdAbend = true;
        cmdOutThread.join();
        return 1;
    } else if (rc) {
        // Build failed, so no more work to do
        using namespace nix;
        printError("build failed with exit code %d", rc);
        cmdAbend = true;
        cmdOutThread.join();
        return rc;
    }

    cmdOutThread.join();

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
} catch (SigHandlerExit & e) {
    return 0;
}

    return 0;
}
