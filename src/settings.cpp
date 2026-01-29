#include "settings.hh"

#include <nix/util/environment-variables.hh>
#include <nix/util/file-system.hh>
#include <nix/util/users.hh>
#include <nix/util/strings.hh>
#include <nix/util/error.hh>
#include <nix/store/globals.hh>

#define NIX_CONF_DIR "/etc/nix"

Settings::Settings()
    : confDir(nix::canonPath(nix::getEnvNonEmpty("NIX_CONF_DIR").value_or(NIX_CONF_DIR)))
    , userConfFiles(getUserConfigFiles())
{}
Settings ourSettings;

void loadConfFile(nix::AbstractConfig & config)
{
    auto applyConfigFile = [&](const nix::Path & path) {
        try {
            std::string contents =  nix::readFile(path);
            config.applyConfig(contents, path);
        } catch (nix::SystemError &) {
        }
    };

    applyConfigFile(nix::settings.nixConfDir + "/nsh.conf");

    auto files = ourSettings.userConfFiles;
    for (auto file = files.rbegin(); file != files.rend(); file++) {
        applyConfigFile(*file);
    }

    auto confEnv = nix::getEnv("NSH_CONFIG");
    if (confEnv.has_value()) {
        config.applyConfig(confEnv.value(), "NSH_CONFIG");
    }
}

std::vector<nix::Path> getUserConfigFiles()
{
    // Use the paths specified in NSH_USER_CONF_FILES if it has been defined
    auto confFiles = nix::getEnv("NSH_USER_CONF_FILES");
    if (confFiles.has_value()) {
        return nix::tokenizeString<std::vector<std::string>>(confFiles.value(), ":");
    }

    // Use the paths specified by the XDG spec
    std::vector<nix::Path> files;
    auto dirs = nix::getConfigDirs();
    for (auto & dir : dirs) {
        files.insert(files.end(), dir + "/nsh.conf");
    }
    return files;
}
