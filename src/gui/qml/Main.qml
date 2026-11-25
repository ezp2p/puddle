import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Qt.labs.platform as Platform
import "components"
import "setup"
import "shell"
import "theme"

ApplicationWindow {
    id: root

    required property var backend

    readonly property bool wide: width >= Theme.wideBreakpoint
    readonly property int margin: wide ? 24 : 12

    visible: true
    width: 900
    height: 600
    minimumWidth: 320
    minimumHeight: 400
    readonly property int alerts: backend.chats.unreadTotal + backend.chats.incomingRequests + backend.chats.incomingGroupInvites +
                                  backend.chats.ponds.mentionsTotal
    title: alerts > 0 ? "(" + alerts + ") Puddle" : "Puddle"

    property string notified: "" // who the last notification was about

    function showWindow() {
        backend.setDockVisible(true)
        show()
        raise()
        requestActivate()
    }

    // Close button hides to the tray. Real quits (tray menu, Cmd+Q, logout)
    // go through, and so does closing when there is no tray to restore from
    // (e.g. stock GNOME), so the app can't become unreachable.
    onClosing: close => {
        if (backend.quitting)
            return
        if (backend.phone) { // Android's back button: back through the app first
            if (shell.visible && shell.back())
                close.accepted = false
            return
        }
        if (tray.available) {
            close.accepted = false
            hide()
            backend.setDockVisible(false)
            if (backend.takeFirstHideNotice())
                tray.showMessage("Puddle is still running",
                                 "Use the tray icon to reopen the window or quit.")
        } else {
            Qt.quit()
        }
    }

    Platform.SystemTrayIcon {
        id: tray
        visible: true
        // Must be a resolved URL: a relative one gives the tray item an empty icon.
        icon.source: Qt.resolvedUrl("../assets/puddle.png")
        // macOS template image: drawn black/white to follow the menu bar's light/dark theme.
        icon.mask: Qt.platform.os === "osx"
        tooltip: "Puddle"
        onMessageClicked: { root.showWindow(); shell.showPerson(root.notified) }
        onActivated: reason => {
            if (reason === Platform.SystemTrayIcon.Trigger
                    || reason === Platform.SystemTrayIcon.DoubleClick)
                root.showWindow()
        }
        menu: Platform.Menu {
            Platform.MenuItem {
                text: qsTr("Show Puddle")
                onTriggered: root.showWindow()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Quit Puddle")
                onTriggered: Qt.quit()
            }
        }
    }

    // the first run: does this app run the owner's Puddle, or use one somewhere else?
    Welcome {
        anchors.fill: parent
        visible: root.backend.mode === ""
        backend: root.backend
        onRecover: setup.recovering = true
    }

    Setup {
        id: setup
        anchors.fill: parent
        visible: root.backend.mode === "here" && root.backend.needsSetup
        backend: root.backend
    }

    // a phone, or this app using a Puddle elsewhere, before that Puddle has let it in (docs/identity.md, "Devices")
    Loader {
        anchors.fill: parent
        active: root.backend.phone && !root.backend.running
        sourceComponent: Pair {
            link: root.backend.phoneLink
            canGoBack: !root.backend.modeFixed
            onBack: root.backend.unchoose()
        }
    }

    Label {
        visible: root.backend.error !== "" && !root.backend.needsSetup && root.backend.mode !== ""
        anchors.centerIn: parent
        width: parent.width - 2 * root.margin
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: root.backend.error
    }

    Shell {
        id: shell
        anchors.fill: parent
        visible: root.backend.running && root.backend.error === "" && !root.backend.needsSetup
        backend: root.backend
    }

    // A notification about `who` (a person, a group, or "" for the window itself): the system's own where there is one (macOS),
    // else the tray's message. Either, clicked, opens the window there.
    function notify(title, body, who) {
        if (backend.notify(title, body, who))
            return
        notified = who
        tray.showMessage(title, body)
    }
    Connections {
        target: root.backend
        function onNotificationClicked(who) { root.showWindow(); shell.showPerson(who) }
    }

    // Tell the owner about new things while the window is hidden. The text of a message is never shown here.
    Connections {
        target: root.backend.chats
        function onNewRequest(userId) {
            if (!root.visible || !root.active)
                root.notify(qsTr("Friend request"), qsTr("%1 would like to be your friend.").arg(root.backend.chats.nameOf(userId)), "")
        }
        function onNewIncoming(userId, unread) {
            if (!root.visible || !root.active)
                root.notify(root.backend.chats.nameOf(userId), qsTr("New message"), userId)
        }
        function onUnreadTotalChanged() { root.updateBadge() }
        function onIncomingRequestsChanged() { root.updateBadge() }
        function onGroupInvitesChanged() { root.updateBadge() }
        function onNewFileRequest(userId, conversationId) {
            root.notify(root.backend.chats.nameOf(userId), qsTr("Asked for a file again"), conversationId)
        }
        function onNewDevice(name) {
            if (!root.visible || !root.active)
                root.notify(qsTr("A phone wants to use your Puddle"), qsTr("Accept \"%1\" in Settings if it is yours.").arg(name), "")
        }
        function onNewGroupInvite(groupId) {
            if (!root.visible || !root.active)
                root.notify(qsTr("Group invitation"), qsTr("You were invited to %1.").arg(root.backend.chats.nameOf(groupId)), "")
        }
    }
    // A Pond's message that mentions us (protocol 15): said unless that channel is in front of the owner, who then sees it there
    Binding { target: root.backend.chats.ponds; property: "windowActive"; value: root.visible && root.active && shell.page === "pond" }
    Connections {
        target: root.backend.chats.ponds
        function onMentioned(pondId, channelId, pondName, channelName) {
            root.notify(pondName, qsTr("You were mentioned in #%1").arg(channelName), "pond:" + pondId + "/" + channelId)
        }
        function onListChanged() { root.updateBadge() }
    }
    // A friend calling: always said, even with the window in front (the call bar shows it there too)
    Connections {
        target: root.backend.chats.calls
        function onRinging(userId, groupId) {
            if (groupId !== "")
                root.notify(qsTr("A call in %1").arg(root.backend.chats.nameOf(groupId)),
                            qsTr("%1 started it. Join in Puddle").arg(root.backend.chats.nameOf(userId)), groupId)
            else
                root.notify(qsTr("%1 is calling").arg(root.backend.chats.nameOf(userId)), qsTr("Answer in Puddle"), userId)
        }
    }
    function updateBadge() {
        backend.setDockBadge(alerts)
    }

    // Cmd/Ctrl+W closes the window (to the tray), as in any chat app
    Shortcut { sequences: [StandardKey.Close]; onActivated: root.close() }

    // The window comes back the size and place it was left.
    Settings {
        category: "window"
        property alias x: root.x
        property alias y: root.y
        property alias width: root.width
        property alias height: root.height
    }
}
