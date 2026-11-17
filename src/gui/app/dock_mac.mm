// macOS only: the app appears in the Dock and Cmd-Tab while its window is open,
// and lives in the menu bar alone while the window is hidden.
#import <AppKit/AppKit.h>
#include <string>

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

// Notifications. Qt's tray message on macOS still goes through NSUserNotificationCenter, which macOS no longer shows, so the app
// posts its own. UNUserNotificationCenter exists only for a process with an app bundle; the tests run bare executables, and for
// them nothing here is set up and puddle_notify says no.
#import <UserNotifications/UserNotifications.h>

static void (*g_clicked)(const char *, void *);
static void *g_clicked_ctx;
static bool g_ready;

@interface PuddleNotifyDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation PuddleNotifyDelegate
// shown even while the app is in front: it only posts when its window is hidden or not focused
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList | UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    NSString *who = response.notification.request.content.userInfo[@"who"];
    if (g_clicked) {
        const std::string w = who ? who.UTF8String : "";
        dispatch_async(dispatch_get_main_queue(), ^{ g_clicked(w.c_str(), g_clicked_ctx); });
    }
    completionHandler();
}
@end

void puddle_notify_init(void (*clicked)(const char *who, void *ctx), void *ctx)
{
    if (g_ready || ![[NSBundle mainBundle] bundleIdentifier] || ![[[NSBundle mainBundle] bundlePath] hasSuffix:@".app"])
        return;
    g_clicked = clicked;
    g_clicked_ctx = ctx;
    static PuddleNotifyDelegate *delegate = [[PuddleNotifyDelegate alloc] init];
    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = delegate;
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
                          completionHandler:^(BOOL granted, NSError *error) {
                              if (error)
                                  NSLog(@"Puddle: notifications unavailable: %@", error.localizedDescription);
                              else
                                  NSLog(granted ? @"Puddle: notifications allowed"
                                                : @"Puddle: notifications are turned off for Puddle in System Settings");
                          }];
    g_ready = true;
}

bool puddle_notify(const char *title, const char *body, const char *who)
{
    if (!g_ready)
        return false;
    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = [NSString stringWithUTF8String:title ? title : ""];
    content.body = [NSString stringWithUTF8String:body ? body : ""];
    content.sound = [UNNotificationSound defaultSound];
    content.userInfo = @{ @"who": [NSString stringWithUTF8String:who ? who : ""] };
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString] content:content trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request
                                                           withCompletionHandler:^(NSError *error) {
                                                               if (error)
                                                                   NSLog(@"Puddle: notification not shown: %@", error.localizedDescription);
                                                           }];
    return true;
}
