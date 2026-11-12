import QtQuick
import QtQuick.Controls
import "../theme"

// "Are you sure?" for what is hard to take back. Call ask(); the action runs only if the red button is pressed.
Dialog {
    id: dlg

    property string message: ""
    property string okText: qsTr("OK")
    property bool destructive: true
    property var action: null

    function ask(heading, text, ok, what) {
        title = heading
        message = text
        okText = ok
        action = what
        open()
    }

    anchors.centerIn: Overlay.overlay
    modal: true
    width: Math.min(380, Overlay.overlay ? Overlay.overlay.width - 24 : 380)

    Label { width: dlg.availableWidth; text: dlg.message; wrapMode: Text.Wrap }

    footer: DialogButtonBox {
        Button { objectName: "confirmCancel"; text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        Button {
            objectName: "confirmOk"
            text: dlg.okText
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            palette.buttonText: dlg.destructive ? Theme.danger : dlg.palette.buttonText
        }
    }

    onAccepted: if (action) { const what = action; action = null; what() }
    onRejected: action = null
}
