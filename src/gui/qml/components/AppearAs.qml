import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Appear as": which of our profiles the other side will see. Choosing one is our consent to show it there, and only there
// (docs/profiles.md). Asked wherever a place begins: sending a friend request, accepting one, joining a group. Always shown while there is
// a profile to choose, as it is what they will see of us.
RowLayout {
    id: row

    required property var chats
    readonly property string profile: box.currentValue ?? "" // "": none, and they see only our id (also while the list is still loading)

    visible: box.count > 0
    Label { text: qsTr("Appear as") }
    ComboBox {
        id: box
        objectName: "appearAs"
        Layout.fillWidth: true
        model: row.chats.profiles
        textRole: "display_name"
        valueRole: "id"
        Accessible.name: qsTr("Profile to show")
    }
}
