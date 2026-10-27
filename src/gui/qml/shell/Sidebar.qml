import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The left-hand list: what is waiting for an answer, then every friend and group, newest first. A "+" starts a conversation. At the bottom,
// who you are: your address and the settings.
Rectangle {
    id: bar

    objectName: "sidebar"
    color: "transparent"   // it is drawn on a card
    required property var chats
    required property var backend
    property string selectedId: ""   // the conversation that is open, if one is
    signal openChat(string id, string kind)
    signal newConversation()
    signal openSettings()
    signal notice(string text)

    // Alt+Up and Alt+Down walk through the friends and groups
    function step(delta) {
        const n = chats.sidebar.count
        let i = -1
        const rows = []
        for (let k = 0; k < n; k++) {
            const kind = chats.sidebarKindAt(k)
            if (kind === "dm" || kind === "group") {
                rows.push(k)
                if (chats.sidebarIdAt(k) === selectedId) i = rows.length - 1
            }
        }
        if (rows.length === 0) return
        i = i < 0 ? (delta > 0 ? 0 : rows.length - 1) : Math.max(0, Math.min(rows.length - 1, i + delta))
        openChat(chats.sidebarIdAt(rows[i]), chats.sidebarKindAt(rows[i]))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                Label { text: qsTr("Direct messages"); font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
                ToolButton {
                    objectName: "newConversationButton"
                    icon.source: Theme.icon("plus"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("New conversation")
                    ToolTip.text: qsTr("New conversation"); ToolTip.visible: hovered
                    onClicked: bar.newConversation()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        ScrollList {
            id: list
            objectName: "dmList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 4
            model: bar.chats.sidebar

            // what needs an answer comes first, under a heading of its own
            delegate: Item {
                id: cell
                required property var model
                required property int index
                readonly property bool firstPending: model.section === "pending" && index === 0
                width: ListView.view.width
                height: (firstPending ? 28 : 0) + Theme.rowHeight

                Label { visible: cell.firstPending; x: 8; y: 8; text: qsTr("Waiting for you"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                SidebarRow {
                    y: cell.firstPending ? 28 : 0
                    width: cell.width
                    model: cell.model
                    chats: bar.chats
                    selected: (model.kind === "dm" || model.kind === "group") && bar.selectedId === model.id
                    onClicked: {
                        if (model.kind === "dm" || model.kind === "group")
                            bar.openChat(model.id, model.kind)
                    }
                    onMenuRequested: (from, x, y) => convMenu.openFor(from, x, y, { id: model.id, kind: model.kind, name: model.name })
                    onJoinRequested: joinConfirm.ask(model.id, model.name)
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.55
                visible: list.count === 0
                text: qsTr("No conversations yet. Use + to start one.")
            }
        }

        // you: who you appear as, your address, settings and the activity log
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 56
            color: "transparent"
            Rectangle { width: parent.width; height: 1; color: Theme.hairline }
            ContextTrigger { onRequested: (x, y) => selfMenu.popup(parent, x, y) }
            Menu {
                id: selfMenu
                MenuItem {
                    objectName: "selfCopyLink"; text: qsTr("Copy my address"); enabled: bar.backend.myLink !== ""
                    onTriggered: { bar.backend.copyText(bar.backend.myLink); bar.notice(qsTr("Address copied")) }
                }
                MenuItem { objectName: "selfSettings"; text: qsTr("Settings"); onTriggered: bar.openSettings() }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8
                Avatar { chats: bar.chats; name: bar.chats.myName; hash: bar.chats.myAvatar; size: 32 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label { text: bar.chats.myName !== "" ? bar.chats.myName : qsTr("You"); font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label {
                        text: bar.chats.connected ? qsTr("Running") : qsTr("Not connected")
                        font.pixelSize: 11
                        opacity: 0.6
                    }
                }
                ToolButton {
                    objectName: "copyLinkButton"
                    icon.source: Theme.icon("link"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Copy my address")
                    ToolTip.text: qsTr("Copy my address"); ToolTip.visible: hovered
                    enabled: bar.backend.myLink !== ""
                    onClicked: { bar.backend.copyText(bar.backend.myLink); bar.notice(qsTr("Address copied")) }
                }
                ToolButton {
                    objectName: "settingsButton"
                    icon.source: Theme.icon("settings"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Settings")
                    ToolTip.text: qsTr("Settings"); ToolTip.visible: hovered
                    onClicked: bar.openSettings()
                }
            }
        }
    }

    ConversationMenu {
        id: convMenu
        chats: bar.chats
        developer: bar.backend.developerMode
        onOpenRequested: (id, kind) => bar.openChat(id, kind)
        onJoinRequested: (id, name) => joinConfirm.ask(id, name)
    }

    // joining shows your address to everyone in the group: say so first
    Dialog {
        id: joinConfirm
        objectName: "joinConfirm"
        property string target: ""
        property string groupName: ""
        function ask(id, name) { target = id; groupName = name; open() }
        anchors.centerIn: Overlay.overlay
        width: 380
        modal: true
        title: qsTr("Join “%1”?").arg(groupName)
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("Everyone in this group will be able to see your address and connect to your Puddle for it, including people who are not your friends.")
        }
        onAccepted: bar.chats.acceptGroup(target)
    }
}
