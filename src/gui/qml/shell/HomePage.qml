import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// What the main area shows when no conversation is open.
Pane {
    id: page

    required property var chats
    property bool narrow: false
    property bool sidebarShown: true
    signal toggleSidebar()
    signal newConversation()
    signal addFriend()

    padding: 0
    background: null

    PanelToggle {
        x: 12; y: (Theme.headerHeight - height) / 2
        glyph: "layout-sidebar"; label: qsTr("Conversations"); active: page.sidebarShown
        onClicked: page.toggleSidebar()
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 360)
        spacing: 12
        Icon { glyph: "message"; size: 40; opacity: 0.4; Layout.alignment: Qt.AlignHCenter }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: 16
            font.bold: true
            text: page.chats.friendCount === 0 && page.chats.conversations.count === 0 ? qsTr("Nothing here yet") : qsTr("Pick a conversation")
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.6
            text: page.chats.friendCount === 0 ? qsTr("Add a friend with their address, and you can talk here.")
                                               : qsTr("Choose someone from the list, or start a new conversation.")
        }
        PrimaryButton {
            id: start
            objectName: "homeNewButton"
            Layout.alignment: Qt.AlignHCenter
            text: page.chats.friendCount === 0 ? qsTr("Add a friend") : qsTr("New conversation")
            onClicked: page.chats.friendCount === 0 ? page.addFriend() : page.newConversation()
        }
    }
}
