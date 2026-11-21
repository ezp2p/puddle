import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A friend ringing (docs/calls.md), floating over the top of the page while the left-hand list is out of sight (a narrow
// window, or the list folded away), so the call is not missed: who, and answer or decline. Everything else about a call,
// and the ringing too when the list is in sight, is in the voice panel at its foot (VoicePanel.qml).
Card {
    id: bar

    required property var chats
    property bool sidebarShown: true
    readonly property var calls: chats.calls
    readonly property bool inGroup: calls.group !== ""

    objectName: "callBar"
    visible: calls.state === "ringing" && !sidebarShown
    implicitWidth: row.implicitWidth + 24
    implicitHeight: row.implicitHeight + 16
    border.color: Theme.primary
    border.width: 2

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 10

        Avatar { chats: bar.chats; name: bar.calls.name; hash: bar.inGroup ? "" : bar.chats.avatarOf(bar.calls.user); size: 28 }
        ColumnLayout {
            spacing: 0
            Label { objectName: "callName"; text: bar.calls.name; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: 220 }
            Label {
                font.pixelSize: 12
                opacity: 0.7
                text: bar.inGroup ? qsTr("%1 started a call").arg(bar.chats.nameOf(bar.calls.user)) : qsTr("is calling you")
            }
        }
        PrimaryButton {
            objectName: "acceptCallButton"
            text: bar.inGroup ? qsTr("Join") : qsTr("Answer")
            icon.source: Theme.icon("phone")
            onClicked: bar.calls.accept()
        }
        Button {
            objectName: "declineCallButton"
            text: qsTr("Decline")
            palette.buttonText: Theme.danger
            onClicked: bar.calls.decline()
        }
    }
}
