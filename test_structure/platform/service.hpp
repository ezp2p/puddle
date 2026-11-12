#pragma once

// Running puddled the way each OS runs a background program, and finding or starting it from the app.
//
//   macOS    a launchd agent (macos/launchd/puddled.plist), signals to stop
//   Linux    a systemd user unit (linux/systemd/puddled.service), signals to stop
//   Windows  a service (StartServiceCtrlDispatcher), or a plain process when started by the app
//
// Only service/main.cpp and app/launcher.cpp use this.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace puddle::platform {

// Run `body` as this OS runs a service, and call `stop` when the OS (or Ctrl-C) asks the program to end.
// Returns what `body` returned.
int runService(const std::function<int()> &body, const std::function<void()> &stop);

// Register puddled to start at login, or remove that.
struct ServiceSpec {
    std::string name = "puddled";
    std::filesystem::path executable;
    std::vector<std::string> args;
};
void installService(const ServiceSpec &spec);
void uninstallService(const std::string &name);

// Whether a puddled already holds this data directory (its lock file), and its process id if so.
std::optional<int> runningPuddle(const std::filesystem::path &dataDir);

// Start puddled as its own process, detached, so it keeps running when the app quits.
int startDetached(const ServiceSpec &spec);

// Where puddled sits next to the app: inside the .app bundle, beside Puddle.exe, or on the PATH.
std::filesystem::path bundledPuddled();

} // namespace puddle::platform
