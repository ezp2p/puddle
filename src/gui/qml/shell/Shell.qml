import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"
import "../chat"
import "../dialogs"
import "../settings"

// The main window: a canvas with the open conversation (or the settings) straight on it, and two cards floating on it. At the left, one card holds
// the rail (where servers will go later) and the list of conversations; at the right, the panel about the open conversation (members, or the
// person's profile). A little circle at the top of the conversation folds each card away. When narrow, the conversation has the whole window and
// the left card slides in over it, as a drawer, when the circle is pressed.
Pane {
    id: shell
    padding: 0
    background: Rectangle { color: Theme.canvas }
    // the owner's text and accent colours reach every control in the window (menus and dialogs are windows of their own, and keep the system's)
    palette.windowText: Theme.text
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent

    required property var backend
    readonly property var chats: backend.chats
    readonly property bool wide: width >= Theme.wideBreakpoint
    // "home" (nothing open), "chat" or "log" (the settings, for now)
    property string page: "home"
    // the side panel (members, or the person's profile): beside the conversation when the window is wide enough, a drawer over it when not
    property bool showContext: true
    property bool showSidebar: true   // the left card, when the window is wide enough to have it beside the conversation
    readonly property bool contextInline: width >= Theme.contextBreakpoint
    readonly property bool contextVisible: contextWanted && (contextInline ? showContext : contextDrawer.opened)
    function toggleContext() {
        if (contextInline) showContext = !showContext
        else if (contextDrawer.opened) contextDrawer.close()
        else contextDrawer.open()
    }
    readonly property bool contextWanted: page === "chat" && chats.current !== ""
    // the conversations: beside the page when wide, a drawer over it when narrow
    readonly property bool sidebarOpen: wide ? showSidebar : sidebarDrawer.opened
    function toggleSidebar() {
        if (wide) showSidebar = !showSidebar
        else if (sidebarDrawer.opened) sidebarDrawer.close()
        else sidebarDrawer.open()
    }
    // Alt+Up / Alt+Down walk through the conversations, whichever of the two holds the list
    function step(delta) { const l = wide ? sidebarInline : sidebarInDrawer; if (l.item) l.item.step(delta) }

    // theme.yaml: the owner's colours and sizes
    Binding { target: Theme; property: "overrides"; value: shell.backend.themeValues }

    // (the page changes first, so closing the chat is not mistaken for the chat going away)
    function goHome() { sidebarDrawer.close(); page = "home"; chats.close() }
    // the conversation most recently open, which the Home button goes back to ({} until one was opened, or once it is gone)
    property var lastChat: ({})
    function openChat(id, kind) { sidebarDrawer.close(); chats.open(id, kind || "dm"); page = "chat"; lastChat = { id: id, kind: kind || "dm" } }
    // the Home button: back into the last conversation; from inside one it stays put (the empty page is for when there is nothing to go back to)
    function homeButton() {
        if (page === "chat") sidebarDrawer.close()
        else if (lastChat.id) openChat(lastChat.id, lastChat.kind)
        else goHome()
    }
    function openLog() { sidebarDrawer.close(); page = "log"; chats.close() }
    function newConversation() { sidebarDrawer.close(); newConv.show(0) }
    function addFriend() { sidebarDrawer.close(); newConv.show(1) }

    // Messages in the open chat count as read only while someone can see the window.
    Binding {
        target: shell.chats
        property: "attentive"
        value: shell.Window.window ? (shell.Window.window.visible && shell.Window.window.active) : false
    }

    Connections {
        target: shell.chats
        // the open conversation went away (the group was left): nothing to show but the empty page
        function onCurrentChanged() { if (shell.page === "chat" && shell.chats.current === "") { shell.lastChat = ({}); shell.goHome() } }
        function onFailed(what) { toast.show(what) }
        function onGroupCreated(id) { shell.openChat(id, "group") }
    }

    // The window cannot reach its own Puddle: say so, rather than showing stale lists as if they were current.
    Rectangle {
        id: offline
        visible: !shell.chats.connected
        z: 5
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: offlineLabel.implicitHeight + 12
        color: Theme.banner
        Label { id: offlineLabel; anchors.centerIn: parent; width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: "white"; text: qsTr("Can't reach your Puddle. Trying again…") }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: offlineLabel.text
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        anchors.topMargin: (offline.visible ? offline.height : 0) + Theme.gap
        spacing: 0

        // the left card, when the window is wide enough to have it beside the page
        Item {
            id: leftSlot
            // (an invisible item gets no width from the layout, so it is shown while it opens)
            visible: shell.wide && (shell.showSidebar || slideWidth > 0)
            clip: true
            // the width is animated here, not on the layout's attached property
            property real slideWidth: shell.wide && shell.showSidebar ? Theme.railWidth + Theme.sidebarWidth + Theme.gap : 0
            Behavior on slideWidth { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
            Layout.fillHeight: true
            Layout.preferredWidth: slideWidth
            Layout.minimumWidth: 0
            Loader {
                id: sidebarInline
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: Theme.railWidth + Theme.sidebarWidth
                active: shell.wide
                sourceComponent: sidebarPanel
            }
        }

        // the page, straight on the canvas
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0

            HomePage {
                objectName: "homePage"
                anchors.fill: parent
                visible: shell.page === "home"
                chats: shell.chats
                narrow: !shell.wide
                sidebarShown: shell.sidebarOpen
                onNewConversation: shell.newConversation()
                onAddFriend: shell.addFriend()
                onToggleSidebar: shell.toggleSidebar()
            }
            LogPage {
                anchors.fill: parent
                visible: shell.page === "log"
                backend: shell.backend
                chats: shell.chats
                narrow: !shell.wide
                sidebarShown: shell.sidebarOpen
                onToggleSidebar: shell.toggleSidebar()
            }
            ChatPage {
                objectName: "chatPage"
                anchors.fill: parent
                visible: shell.page === "chat"
                chats: shell.chats
                narrow: !shell.wide
                sidebarShown: shell.sidebarOpen
                contextShown: shell.contextVisible
                onToggleSidebar: shell.toggleSidebar()
                onToggleContext: shell.toggleContext()
            }
        }

        // the right card, beside the conversation
        Item {
            id: rightSlot
            readonly property bool wanted: shell.contextWanted && shell.contextInline && shell.showContext
            visible: wanted || slideWidth > 0   // (an invisible item gets no width from the layout, so it has to be shown to open)
            clip: true
            Layout.fillHeight: true
            property real slideWidth: wanted ? Theme.contextWidth + Theme.gap : 0
            Behavior on slideWidth { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
            Layout.preferredWidth: slideWidth
            Card {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: Theme.contextWidth
                Loader {
                    anchors.fill: parent
                    active: rightSlot.visible
                    sourceComponent: contextPanel
                }
            }
        }
    }

    // the rail and the conversations, on a card
    Component {
        id: sidebarPanel
        Card {
            function step(delta) { sidebar.step(delta) }

            Rectangle { x: Theme.railWidth; width: 1; height: parent.height; color: Theme.hairline }

            Column {
                objectName: "rail"
                width: Theme.railWidth
                y: 14
                spacing: 8
                Rectangle {
                    id: home
                    objectName: "homeButton"
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 44
                    height: 44
                    radius: shell.page === "chat" ? 22 : 16
                    color: shell.page !== "chat" || homeHover.hovered ? Theme.accent : Theme.tint(Theme.text, Theme.selected)
                    Behavior on radius { NumberAnimation { duration: 120 } }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Home")
                    Accessible.onPressAction: shell.homeButton()
                    Image {
                        anchors.centerIn: parent
                        width: 26; height: 26
                        source: "../../assets/puddle.png"
                        sourceSize: Qt.size(52, 52)
                        smooth: true
                    }
                    HoverHandler { id: homeHover }
                    TapHandler { onTapped: shell.homeButton() }
                    ToolTip.visible: homeHover.hovered
                    ToolTip.text: qsTr("Home")
                    // the unread mark, like Discord's red badge
                    Rectangle {
                        visible: shell.chats.unreadTotal + shell.chats.incomingRequests + shell.chats.incomingGroupInvites > 0
                        anchors.right: parent.right; anchors.bottom: parent.bottom
                        anchors.rightMargin: -4; anchors.bottomMargin: -4
                        width: Math.max(18, badge.implicitWidth + 8); height: 18; radius: 9
                        color: Theme.badge
                        border.width: 3; border.color: Theme.card
                        Label {
                            id: badge
                            anchors.centerIn: parent
                            text: shell.chats.unreadTotal + shell.chats.incomingRequests + shell.chats.incomingGroupInvites
                            color: "white"; font.pixelSize: 10; font.bold: true
                        }
                    }
                }
                Rectangle { width: 28; height: 2; radius: 1; color: Theme.hairline; anchors.horizontalCenter: parent.horizontalCenter }
            }

            Sidebar {
                id: sidebar
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: Theme.railWidth + 1
                anchors.right: parent.right
                chats: shell.chats
                backend: shell.backend
                selectedId: shell.page === "chat" ? shell.chats.current : ""
                onOpenChat: (id, kind) => shell.openChat(id, kind)
                onNewConversation: shell.newConversation()
                onOpenSettings: shell.openLog()
                onNotice: text => toast.show(text)
            }
        }
    }
    // when the window is narrow: slides in over the page
    Drawer {
        id: sidebarDrawer
        objectName: "sidebarDrawer"
        edge: Qt.LeftEdge
        width: Math.min(Theme.railWidth + Theme.sidebarWidth + 2 * Theme.gap, shell.width * 0.92)
        height: shell.height
        dragMargin: 0   // opened by its button, not by dragging in from the edge
        modal: true
        padding: 0
        background: null
        enter: Transition { NumberAnimation { property: "position"; to: 1; duration: 220; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "position"; to: 0; duration: 180; easing.type: Easing.InCubic } }
        Loader {
            id: sidebarInDrawer
            anchors.fill: parent
            anchors.margins: Theme.gap
            active: !shell.wide
            sourceComponent: sidebarPanel
        }
    }

    Component {
        id: contextPanel
        ContextBar {
            chats: shell.chats
            developer: shell.backend.developerMode
            closable: !shell.contextInline
            onCloseRequested: contextDrawer.close()
            onOpenChat: id => { contextDrawer.close(); shell.openChat(id, "dm") }
            onAddPeople: newConv.showAdd(shell.chats.current, shell.chats.currentName)
            onRenameGroup: groupTools.rename(shell.chats.current, shell.chats.currentName)
        }
    }
    // when there is no room beside the conversation: slides in over it from the right, on a card like the one it would sit on
    Drawer {
        id: contextDrawer
        objectName: "contextDrawer"
        edge: Qt.RightEdge
        width: Math.min(Theme.contextWidth + 2 * Theme.gap, shell.width * 0.92)
        height: shell.height
        dragMargin: 0   // opened by its button, not by dragging in from the edge
        modal: true
        padding: 0
        background: null
        enter: Transition { NumberAnimation { property: "position"; to: 1; duration: 220; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "position"; to: 0; duration: 180; easing.type: Easing.InCubic } }
        Card {
            anchors.fill: parent
            anchors.margins: Theme.gap
            Loader { anchors.fill: parent; active: contextDrawer.opened || contextDrawer.visible; sourceComponent: contextPanel }
        }
    }
    ConversationMenu { id: groupTools; chats: shell.chats }

    NewConversationDialog {
        id: newConv
        objectName: "newConversationDialog"
        chats: shell.chats
        backend: shell.backend
        onStartDm: id => shell.openChat(id, "dm")
    }

    Shortcut { sequences: [StandardKey.New]; onActivated: shell.newConversation() }
    // Alt+Up / Alt+Down walk through the conversations, as in Discord
    Shortcut { sequences: ["Alt+Up"]; onActivated: shell.step(-1) }
    Shortcut { sequences: ["Alt+Down"]; onActivated: shell.step(1) }
    // fold the cards away: Ctrl+B the conversations (as VS Code's sidebar), Ctrl+U the members or profile (as Discord's member list)
    Shortcut { sequences: ["Ctrl+B"]; onActivated: shell.toggleSidebar() }
    Shortcut { sequences: ["Ctrl+U"]; enabled: shell.contextWanted; onActivated: shell.toggleContext() }
    // a way to get somewhere from outside (a notification was clicked)
    function showPerson(id) { id === "" ? goHome() : openChat(id) }

    // a short message at the bottom
    Rectangle {
        id: toast
        property string message: ""
        function show(text) { message = text; opacity = 1; hideTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: Math.min(parent.width - 32, toastLabel.implicitWidth + 32)
        height: toastLabel.implicitHeight + 20
        radius: Theme.radiusLarge
        color: Theme.toast
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: message
        Label { id: toastLabel; anchors.centerIn: parent; width: parent.width - 32; text: toast.message; color: "white"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
        Timer { id: hideTimer; interval: 4000; onTriggered: toast.opacity = 0 }
    }
}
