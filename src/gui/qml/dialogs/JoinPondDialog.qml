import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The "+" under the Ponds on the rail: paste the pond:// invite someone gave you, choose who the members will see you as, and join.
Dialog {
    id: dlg

    required property var chats
    readonly property var ponds: chats.ponds

    function show() { link.text = ""; open(); link.forceActiveFocus() }
    function join() { if (link.text.trim() !== "" && !ponds.joining) ponds.join(link.text, appearAs.profile) }

    objectName: "joinPondDialog"
    title: qsTr("Join a Pond")
    anchors.centerIn: Overlay.overlay
    modal: true
    width: Math.min(440, Overlay.overlay ? Overlay.overlay.width - 24 : 440)
    footer: null

    Connections {
        target: dlg.ponds
        function onJoined() { dlg.close() }
    }

    ColumnLayout {
        width: dlg.availableWidth
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.75
            text: qsTr("A Pond is a community server with channels. Paste the invite you were given.")
        }
        TextField {
            id: link
            objectName: "pondLink"
            Layout.fillWidth: true
            placeholderText: "pond://pond.example.org?invite=…"
            onAccepted: dlg.join()
            FieldMenu {}
        }
        AppearAs { id: appearAs; chats: dlg.chats; Layout.fillWidth: true }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: dlg.close() }
            PrimaryButton {
                objectName: "joinPondButton"
                text: dlg.ponds.joining ? qsTr("Joining…") : qsTr("Join")
                enabled: link.text.trim() !== "" && !dlg.ponds.joining
                onClicked: dlg.join()
            }
        }
    }
}
