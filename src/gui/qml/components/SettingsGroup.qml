import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// A group of settings on a settings page: a small heading, a card holding them, and an optional note under it. What goes
// inside is laid out in a column, each item as wide as the card when it asks to fill (Layout.fillWidth).
ColumnLayout {
    id: group

    property string title
    property string note
    default property alias content: body.data

    spacing: 8
    Label { visible: group.title !== ""; text: group.title.toUpperCase(); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + 32
        radius: Theme.radiusLarge
        color: Theme.tint(Theme.text, 0.035)
        border.width: 1
        border.color: Theme.hairline
        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 8
        }
    }
    Label { visible: group.note !== ""; Layout.fillWidth: true; wrapMode: Text.Wrap; text: group.note; font.pixelSize: 12; opacity: 0.6 }
}
