import QtQuick
import QtQuick.Controls
import "../theme"

// The right-click menu of a line of the sidebar, by what it is: a friend (open, remove, block), a group (open, rename, leave), a friend request
// (accept, decline, block), an invitation (join, decline) or a request we sent (cancel). Every row shares one: call openFor() from the row.
// What is hard to take back asks first.
Item {
    id: root

    required property var chats
    property bool developer: false // Developer Mode: also offer the id
    signal openRequested(string id, string kind)
    signal joinRequested(string id, string name) // for an invitation: the caller says what joining costs, then joins

    property string convId: ""
    property string convKind: "dm"
    property string convName: ""
    readonly property bool isGroup: convKind === "group"
    readonly property bool isDm: convKind === "dm"
    readonly property bool isRequest: convKind === "request"
    readonly property bool isInvite: convKind === "invite"
    readonly property bool isSent: convKind === "sent"

    // the rename dialog on its own, for a button that is not a menu item
    function rename(id, name) { renameDialog.start(id, name) }

    // `conv` is { id, kind, name }; the menu appears at (x, y) in `parentItem`
    function openFor(parentItem, x, y, conv) {
        convId = conv.id
        convKind = conv.kind || "dm"
        convName = conv.name || ""
        menu.popup(parentItem, x, y)
    }

    Menu {
        id: menu
        MenuItem {
            objectName: "convOpen"; text: qsTr("Open")
            visible: root.isDm || root.isGroup; height: visible ? implicitHeight : 0
            onTriggered: root.openRequested(root.convId, root.convKind)
        }
        MenuItem {
            objectName: "convAccept"; text: qsTr("Accept")
            visible: root.isRequest; height: visible ? implicitHeight : 0
            onTriggered: root.chats.accept(root.convId)
        }
        MenuItem {
            objectName: "convJoin"; text: qsTr("Join…")
            visible: root.isInvite; height: visible ? implicitHeight : 0
            onTriggered: root.joinRequested(root.convId, root.convName)
        }
        MenuItem {
            objectName: "convDecline"; text: qsTr("Decline")
            visible: root.isRequest || root.isInvite; height: visible ? implicitHeight : 0
            onTriggered: root.isInvite ? root.chats.declineGroup(root.convId) : root.chats.decline(root.convId)
        }
        MenuItem {
            objectName: "convCancel"; text: qsTr("Cancel request")
            visible: root.isSent; height: visible ? implicitHeight : 0
            onTriggered: root.chats.cancel(root.convId)
        }
        MenuItem {
            objectName: "convRename"; text: qsTr("Rename group…")
            visible: root.isGroup; height: visible ? implicitHeight : 0
            onTriggered: renameDialog.start(root.convId, root.convName)
        }
        MenuItem {
            objectName: "convCopyId"; text: qsTr("Copy ID")
            visible: root.developer && !root.isSent; height: visible ? implicitHeight : 0
            onTriggered: root.chats.copy(root.convId)
        }
        MenuSeparator { visible: root.isDm || root.isGroup || root.isRequest; height: visible ? implicitHeight : 0 }
        MenuItem {
            objectName: "convRemove"; text: qsTr("Remove friend…"); palette.text: Theme.danger
            visible: root.isDm; height: visible ? implicitHeight : 0
            onTriggered: confirm.ask(qsTr("Remove friend"),
                                     qsTr("Remove %1 from your friends? The connection between you is closed and they are told. Your messages stay.").arg(root.convName),
                                     qsTr("Remove"), () => root.chats.cancel(root.convId))
        }
        MenuItem {
            objectName: "convBlock"; text: qsTr("Block…"); palette.text: Theme.danger
            visible: root.isDm || root.isRequest; height: visible ? implicitHeight : 0
            onTriggered: confirm.ask(qsTr("Block"),
                                     qsTr("Block %1? They are cut off at once and cannot ask again until you unblock them.").arg(root.convName),
                                     qsTr("Block"), () => root.chats.block(root.convId))
        }
        MenuItem {
            objectName: "convLeave"; text: qsTr("Leave group…"); palette.text: Theme.danger
            visible: root.isGroup; height: visible ? implicitHeight : 0
            onTriggered: confirm.ask(qsTr("Leave group"),
                                     qsTr("Leave “%1”? The others will be told. You can only come back if someone invites you again.").arg(root.convName),
                                     qsTr("Leave"), () => root.chats.leaveGroup(root.convId))
        }
    }

    ConfirmDialog { id: confirm; objectName: "convConfirm" }

    Dialog {
        id: renameDialog
        objectName: "renameDialog"
        property string target: ""
        function start(id, name) {
            target = id
            nameField.text = name
            open()
            nameField.forceActiveFocus()
            nameField.selectAll()
        }
        anchors.centerIn: Overlay.overlay
        modal: true
        width: Math.min(380, Overlay.overlay ? Overlay.overlay.width - 24 : 380)
        title: qsTr("Rename group")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(() => nameField.text.trim() !== "")
        TextField {
            id: nameField
            objectName: "renameDialogField"
            width: parent.width
            selectByMouse: true
            onTextChanged: if (length > 100) remove(100, length)
            onAccepted: if (text.trim() !== "") renameDialog.accept()
            FieldMenu {}
        }
        onAccepted: root.chats.renameGroup(target, nameField.text)
    }
}
