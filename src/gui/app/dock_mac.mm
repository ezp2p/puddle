// macOS only: the app appears in the Dock and Cmd-Tab while its window is open,
// and lives in the menu bar alone while the window is hidden.
#import <AppKit/AppKit.h>

#include "dock.h"

void puddle_set_dock_visible(bool visible)
{
    NSApplication *app = NSApp;
    if (!app)
        return;
    [app setActivationPolicy:visible ? NSApplicationActivationPolicyRegular
                                     : NSApplicationActivationPolicyAccessory];
    if (visible)
        [app activateIgnoringOtherApps:YES];
}

void puddle_set_dock_badge(int count)
{
    NSApplication *app = NSApp;
    if (!app)
        return;
    [[app dockTile] setBadgeLabel:count > 0 ? [NSString stringWithFormat:@"%d", count] : nil];
}
