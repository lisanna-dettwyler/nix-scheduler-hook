#pragma once

#include "settings.hh"

#include <nix/util/fmt.hh>
#include <nix/store/store-api.hh>

#include <boost/algorithm/string/join.hpp>

#define PATH_VAR "PATH=/run/current-system/sw/bin/:/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin"

static std::string genScript(nix::StorePath drvPath, std::string rootPath)
{
    return nix::fmt(
        "#!/bin/sh\n"
        "while ! nix-store --store '%s' --query --hash %s/%s >/dev/null 2>&1; do sleep 0.1; done;"
        "nix-store --store '%s' --realise %s/%s --option system-features '%s' --add-root %s --quiet;"
        "rc=$?;"
        "echo '@nsh done' >&2;"
        "exit $rc",
        ourSettings.remoteStore.get(),
        ourSettings.storeDir.get(), std::string(drvPath.to_string()),
        ourSettings.remoteStore.get(),
        ourSettings.storeDir.get(), std::string(drvPath.to_string()),
        boost::algorithm::join(ourSettings.systemFeatures.get(), " "),
        rootPath
    );
}
