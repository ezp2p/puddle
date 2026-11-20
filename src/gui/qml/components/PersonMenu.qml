import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// The menu a right-click on someone's name or picture opens, as Discord's does: Profile, Message (a friend), and in a Pond, for
// an operator or administrator who outranks them, Mute or Unmute, Kick and Ban. Only what the viewer may do is shown; the Pond
// decides in the end (the Pond's docs/design.md, "Roles"). popup(item, x, y, card): `card` is Ponds.card(id) or Chats.card(id),
// with `friend` said.
Item {
    id: pm

    property var chats
    property var ponds: null          // in a Pond; null in a Puddle chat
    property bool developer: false    // Developer Mode: Copy ID
    property var info: ({})
    signal profileRequested(var info)
    signal messageRequested(string userId)

    readonly property string userId: info.id || ""
    readonly property string name: info.name || ""
    readonly property bool mine: info.isMe === true
    // may act on them: a Pond where we outrank them (never ourselves, never an operator). Asked as the menu opens: ranks change.
    property bool canMod: false

    function popup(item, x, y, data) {
        info = data
        canMod = ponds !== null && !!data.id && ponds.canActOn(data.id)
        menu.popup(item, x, y)
    }

    Menu {
        id: menu
        objectName: "personMenu"
        MenuItem { objectName: "personProfile"; text: qsTr("Profile"); onTriggered: pm.profileRequested(pm.info) }
        MenuItem {
            objectName: "personMessage"; text: qsTr("Message")
            visible: pm.info.friend === true && !pm.mine; height: visible ? implicitHeight : 0
            onTriggered: pm.messageRequested(pm.userId)
        }
        MenuItem {
            objectName: "personCopyId"; text: qsTr("Copy ID")
            visible: pm.developer; height: visible ? implicitHeight : 0
            onTriggered: pm.chats.copy(pm.userId)
        }
        MenuSeparator { visible: pm.canMod; height: visible ? implicitHeight : 0 }
        MenuItem {
            objectName: "personMute"; text: qsTr("Mute %1…").arg(pm.name)
            visible: pm.canMod && pm.ponds.canMute && pm.info.muted !== true; height: visible ? implicitHeight : 0
            onTriggered: muteDialog.ask()
        }
        MenuItem {
            objectName: "personUnmute"; text: qsTr("Unmute %1").arg(pm.name)
            visible: pm.canMod && pm.ponds.canMute && pm.info.muted === true; height: visible ? implicitHeight : 0
            onTriggered: pm.ponds.unmute(pm.userId)
        }
        MenuItem {
            objectName: "personKick"; text: qsTr("Kick %1").arg(pm.name); palette.text: Theme.danger
            visible: pm.canMod; height: visible ? implicitHeight : 0
            onTriggered: confirm.ask(qsTr("Kick %1?").arg(pm.name), qsTr("They leave the Pond now. They can come back with an invite."),
                                     qsTr("Kick"), (id => () => pm.ponds.kick(id))(pm.userId))
        }
        MenuItem {
            objectName: "personBan"; text: qsTr("Ban %1…").arg(pm.name); palette.text: Theme.danger
            visible: pm.canMod; height: visible ? implicitHeight : 0
            onTriggered: banDialog.ask()
        }
    }

    ConfirmDialog { id: confirm }

    // muting is for a while, as Discord's timeouts are, or until they are unmuted
    Dialog {
        id: muteDialog
        property string member: ""
        function ask() { member = pm.userId; duration.currentIndex = 2; open() }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(400, parent ? parent.width - 24 : 400)
        title: qsTr("Mute %1?").arg(pm.name)
        ColumnLayout {
            width: muteDialog.availableWidth
            spacing: 8
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("They stay in the Pond and can read it, but can't write, and aren't heard in voice channels.") }
            ComboBox {
                id: duration
                objectName: "muteDuration"
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                model: [
                    { text: qsTr("For 60 seconds"), value: 60 }, { text: qsTr("For 5 minutes"), value: 300 }, { text: qsTr("For 10 minutes"), value: 600 },
                    { text: qsTr("For an hour"), value: 3600 }, { text: qsTr("For a day"), value: 86400 }, { text: qsTr("For a week"), value: 604800 },
                    { text: qsTr("Until I unmute them"), value: 0 }
                ]
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { objectName: "confirmMute"; text: qsTr("Mute"); palette.buttonText: Theme.danger; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
        onAccepted: pm.ponds.mute(member, duration.currentValue)
    }

    // banning asks why (optional), which the other operators and administrators see in Server Settings, Bans
    Dialog {
        id: banDialog
        property string member: ""
        function ask() { member = pm.userId; reason.text = ""; open(); reason.forceActiveFocus() }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(400, parent ? parent.width - 24 : 400)
        title: qsTr("Ban %1?").arg(pm.name)
        ColumnLayout {
            width: banDialog.availableWidth
            spacing: 8
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("They leave the Pond now, and can't join again until they are unbanned.") }
            TextField { id: reason; objectName: "personBanReason"; Layout.fillWidth: true; placeholderText: qsTr("Reason (optional)"); maximumLength: 500 }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { objectName: "personConfirmBan"; text: qsTr("Ban"); palette.buttonText: Theme.danger; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
        onAccepted: pm.ponds.ban(member, reason.text)
    }
}
