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
    readonly property int alerts: backend.chats.unreadTotal + backend.chats.incomingRequests + backend.chats.incomingGroupInvites
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

    Setup {
        anchors.fill: parent
        visible: root.backend.needsSetup
        backend: root.backend
    }

    Label {
        visible: root.backend.error !== "" && !root.backend.needsSetup
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

    // Tell the owner about new things while the window is hidden. The text of a message is never shown here.
    Connections {
        target: root.backend.chats
        function onNewRequest(userId) {
            if (!root.visible || !root.active) {
                root.notified = ""
                tray.showMessage(qsTr("Friend request"),
                                 qsTr("%1 would like to be your friend.").arg(root.backend.chats.nameOf(userId)))
            }
        }
        function onNewIncoming(userId, unread) {
            if (!root.visible || !root.active) {
                root.notified = userId
                tray.showMessage(root.backend.chats.nameOf(userId), qsTr("New message"))
            }
        }
        function onUnreadTotalChanged() { root.updateBadge() }
        function onIncomingRequestsChanged() { root.updateBadge() }
        function onGroupInvitesChanged() { root.updateBadge() }
        function onNewGroupInvite(groupId) {
            if (!root.visible || !root.active) {
                root.notified = ""
                tray.showMessage(qsTr("Group invitation"), qsTr("You were invited to %1.").arg(root.backend.chats.nameOf(groupId)))
            }
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
