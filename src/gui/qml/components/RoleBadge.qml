import QtQuick
import QtQuick.Controls
import "../theme"

// A member's role in a Pond, as a small label beside their name: Operator or Admin. Nothing for an ordinary member.
Rectangle {
    id: badge

    property string role

    visible: role === "operator" || role === "administrator"
    implicitWidth: label.implicitWidth + 12
    implicitHeight: label.implicitHeight + 4
    radius: height / 2
    color: role === "operator" ? Theme.tint(Theme.accent, 0.25) : Theme.tint(Theme.primary, 0.25)
    Accessible.role: Accessible.StaticText
    Accessible.name: label.text
    Label {
        id: label
        anchors.centerIn: parent
        text: badge.role === "operator" ? qsTr("Operator") : qsTr("Admin")
        font.pixelSize: 10
        font.bold: true
    }
}
