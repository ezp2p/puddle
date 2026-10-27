#pragma once

// Show or hide the app's Dock icon (macOS). No-op elsewhere.
// The unread count on the Dock icon (0 clears it).
// Notifications through the system's own centre (macOS: UNUserNotificationCenter, which asks the owner's permission once).
// puddle_notify_init asks for that permission and says what to call when one is clicked, with the `who` it was posted with.
// puddle_notify returns false where there is no native way (another OS, or not running from an app bundle): the caller falls back
// to the tray's message.
#ifdef __APPLE__
void puddle_set_dock_visible(bool visible);
void puddle_set_dock_badge(int count);
void puddle_notify_init(void (*clicked)(const char *who, void *ctx), void *ctx);
bool puddle_notify(const char *title, const char *body, const char *who);
#else
inline void puddle_set_dock_visible(bool) {}
inline void puddle_set_dock_badge(int) {}
inline void puddle_notify_init(void (*)(const char *, void *), void *) {}
inline bool puddle_notify(const char *, const char *, const char *) { return false; }
#endif
