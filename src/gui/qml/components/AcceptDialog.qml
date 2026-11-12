import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// Saying yes to a friend request: first, which of our profiles they will see, as the request dialog asks the one who sends it. Without a
// choice they would see only our id. Call ask(); the request is accepted only if Accept is pressed. With no profile to choose, it accepts at once.
Dialog {
    id: dlg

    objectName: "acceptConfirm"
    required property var chats
    property string target: ""
    property string personName: ""

    function ask(id, name) {
        if (chats.profiles.count === 0) { chats.accept(id); return }
        target = id
        personName = name || ""
        open()
    }

    anchors.centerIn: Overlay.overlay
    modal: true
    width: Math.min(380, Overlay.overlay ? Overlay.overlay.width - 24 : 380)
    title: personName !== "" ? qsTr("Accept %1?").arg(personName) : qsTr("Accept this request?")

    ColumnLayout {
        width: dlg.availableWidth
        spacing: 12
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Choose how they will see you.")
        }
        AppearAs { id: appearAs; chats: dlg.chats; Layout.fillWidth: true }
    }

    footer: DialogButtonBox {
        Button { objectName: "acceptCancel"; text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        Button { objectName: "acceptOk"; text: qsTr("Accept"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
    }

    onAccepted: chats.accept(target, appearAs.profile)
}
