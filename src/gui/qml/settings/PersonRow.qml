import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// One person in the Friends lists: who, what state, and what can be done.
// `mode` is "friend", "pending" or "blocked".
Rectangle {
    id: row

    required property var chats
    required property string mode
    property string userId: ""
    property string name: ""
    property string avatar: ""
    property string tint: ""
    property string streamState: ""
    property string note: ""
    property string error: ""
    property bool provisional: false
    property bool certChanged: false
    property bool developer: false // Developer Mode: show what is normally hidden
    property var confirmDialog: null // a ConfirmDialog to ask in; without one the action just happens
    signal message()

    function ask(heading, text, ok, what) {
        if (confirmDialog) confirmDialog.ask(heading, text, ok, what)
        else what()
    }

    implicitHeight: Math.max(col.implicitHeight, 40) + 20
    color: hover.hovered ? Theme.tint(palette.text, Theme.hover) : "transparent"
    radius: Theme.radiusLarge
    Accessible.role: Accessible.ListItem
    Accessible.name: name

    HoverHandler { id: hover }
    ContextTrigger { onRequested: (x, y) => menu.popup(row, x, y) }
    TapHandler {
        enabled: row.mode === "friend"
        onDoubleTapped: row.message()
    }

    // what can be done about this person, by what they are to us
    Menu {
        id: menu
        MenuItem {
            objectName: "personMessage"; text: qsTr("Message"); visible: row.mode === "friend"; height: visible ? implicitHeight : 0
            onTriggered: row.message()
        }
        MenuItem {
            objectName: "personAccept"; text: qsTr("Accept"); visible: row.streamState === "pending_in"; height: visible ? implicitHeight : 0
            onTriggered: chats.accept(row.userId)
        }
        MenuItem {
            objectName: "personDecline"; text: qsTr("Decline"); visible: row.streamState === "pending_in"; height: visible ? implicitHeight : 0
            onTriggered: chats.decline(row.userId)
        }
        MenuItem {
            objectName: "personCancel"; text: qsTr("Cancel request"); visible: row.streamState === "pending_out"; height: visible ? implicitHeight : 0
            onTriggered: chats.cancel(row.userId)
        }
        MenuItem {
            objectName: "personUnblock"; text: qsTr("Unblock"); visible: row.mode === "blocked"; height: visible ? implicitHeight : 0
            onTriggered: chats.unblock(row.userId)
        }
        MenuItem {
            objectName: "personCopyId"; text: qsTr("Copy user ID"); visible: row.developer && !row.provisional; height: visible ? implicitHeight : 0
            onTriggered: chats.copy(row.userId)
        }
        MenuSeparator { visible: row.mode === "friend" || (row.mode === "pending" && !row.provisional); height: visible ? implicitHeight : 0 }
        MenuItem {
            objectName: "personRemove"; text: qsTr("Remove friend…"); palette.text: Theme.danger
            visible: row.mode === "friend"; height: visible ? implicitHeight : 0
            onTriggered: row.ask(qsTr("Remove friend"),
                                 qsTr("Remove %1 from your friends? The connection between you is closed and they are told. Your messages stay.").arg(row.name),
                                 qsTr("Remove"), () => chats.cancel(row.userId))
        }
        MenuItem {
            objectName: "personBlock"; text: qsTr("Block…"); palette.text: Theme.danger
            visible: row.mode !== "blocked" && !row.provisional; height: visible ? implicitHeight : 0
            onTriggered: row.ask(qsTr("Block"),
                                 qsTr("Block %1? They are cut off at once and cannot ask again until you unblock them.").arg(row.name),
                                 qsTr("Block"), () => chats.block(row.userId))
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        anchors.topMargin: 10
        spacing: 12

        Avatar { chats: row.chats; name: row.name; hash: row.avatar; tint: row.tint; size: 40; Layout.alignment: Qt.AlignTop }

        ColumnLayout {
            id: col
            Layout.fillWidth: true
            spacing: 2
            Label { text: row.name; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            Label {
                Layout.fillWidth: true
                opacity: 0.65
                font.pixelSize: 12
                elide: Text.ElideRight
                text: row.mode === "blocked" ? qsTr("Blocked")
                      : row.streamState === "pending_in" ? qsTr("Incoming friend request")
                      : row.streamState === "pending_out" ? qsTr("Outgoing friend request")
                      : row.streamState === "closed" ? qsTr("Stream closed")
                      : ""
                visible: text !== ""
            }
            Label {
                visible: row.note !== "" && row.streamState === "pending_in"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "“" + row.note + "”"
            }
            // Someone we do not know can claim any name. The code and the id are what to compare with them.
            // Developer Mode only: who they claim to be, and the code derived from it
            Label {
                objectName: "codeLabel"
                visible: row.developer && !row.provisional
                Layout.fillWidth: true
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
                text: qsTr("ID %1  ·  Code %2").arg(row.userId).arg(chats.shortCode(row.userId))
            }
            Label {
                visible: row.error !== "" && row.mode !== "blocked"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: 12
                text: row.error
            }
            RowLayout {
                visible: row.certChanged
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.warning
                    font.pixelSize: 12
                    text: qsTr("Their certificate changed. Nothing is sent until you trust it.")
                }
                Button { text: qsTr("Trust"); Layout.preferredWidth: 72; onClicked: chats.trust(row.userId) }
            }
        }

        // actions
        RoundButton {
            objectName: "messageButton"
            visible: row.mode === "friend"
            icon.source: Theme.icon("message")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Message")
            ToolTip.text: qsTr("Message"); ToolTip.visible: hovered
            onClicked: row.message()
        }
        RoundButton {
            objectName: "acceptButton"
            visible: row.streamState === "pending_in"
            icon.source: Theme.icon("check")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Accept")
            ToolTip.text: qsTr("Accept"); ToolTip.visible: hovered
            onClicked: chats.accept(row.userId)
        }
        RoundButton {
            visible: row.streamState === "pending_in"
            icon.source: Theme.icon("x")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Decline")
            ToolTip.text: qsTr("Decline"); ToolTip.visible: hovered
            onClicked: chats.decline(row.userId)
        }
        RoundButton {
            visible: row.streamState === "pending_out"
            icon.source: Theme.icon("x")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Cancel request")
            ToolTip.text: qsTr("Cancel request"); ToolTip.visible: hovered
            onClicked: chats.cancel(row.userId)
        }
        RoundButton {
            objectName: "personMore"
            visible: row.mode === "friend" || row.streamState === "pending_in" || row.streamState === "pending_out"
            icon.source: Theme.icon("dots")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("More")
            onClicked: menu.popup(this, 0, height)
        }
        Button {
            visible: row.mode === "blocked"
            text: qsTr("Unblock")
            onClicked: chats.unblock(row.userId)
        }
    }
}
