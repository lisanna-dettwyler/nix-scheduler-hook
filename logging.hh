#include <memory>
#include <string>

#include <nix/util/serialise.hh>
#include <nix/store/globals.hh>
#include <nix/store/build-result.hh>

#define NSH_BUILD_LOG_TERMINATOR "@nsh done"

bool handleOutput(std::ostream & logOs, std::string_view data)
{
    using namespace nix;
    static unsigned long logSize = 0;
    static size_t currentLogLinePos = 0;
    static std::string currentLogLine;
    static std::string currentHookLine;
    logSize += data.size();
    if (settings.maxLogSize && logSize > settings.maxLogSize) {
        throw BuildError(
            BuildResult::LogLimitExceeded,
            "wrote more than %d bytes of log output",
            settings.maxLogSize);
    }

    for (auto c : data)
        if (c == '\r')
            currentLogLinePos = 0;
        else if (c == '\n') {
            if (currentLogLine != NSH_BUILD_LOG_TERMINATOR) {
                logOs << currentLogLine << '\n';
                currentLogLine.clear();
                currentLogLinePos = 0;
            } else return true;
        } else {
            if (currentLogLinePos >= currentLogLine.size())
                currentLogLine.resize(currentLogLinePos + 1);
            currentLogLine[currentLogLinePos++] = c;
        }

    return false;
}