// platform/service.hpp for macOS: launchd, and signals.

#include "platform/service.hpp"

#include <csignal>

namespace puddle::platform {

int runService(const std::function<int()> &body, const std::function<void()> &stop)
{
    // SIGTERM (launchd, kill) and SIGINT (Ctrl-C) call `stop`, from a thread that waits on them with sigwait,
    // so `stop` never runs inside a signal handler.
    return body();
}

// installService: write ~/Library/LaunchAgents/<name>.plist from launchd/puddled.plist, `launchctl bootstrap`
// runningPuddle: posix/lock.cpp
// startDetached: posix_spawn with a new session (setsid), so quitting the app does not end it
// bundledPuddled: Contents/MacOS/puddled inside the app bundle

} // namespace puddle::platform
