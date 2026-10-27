#pragma once

// Show or hide the app's Dock icon (macOS). No-op elsewhere.
// The unread count on the Dock icon (0 clears it).
#ifdef __APPLE__
void puddle_set_dock_visible(bool visible);
void puddle_set_dock_badge(int count);
#else
inline void puddle_set_dock_visible(bool) {}
inline void puddle_set_dock_badge(int) {}
#endif
