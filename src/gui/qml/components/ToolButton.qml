import QtQuick
import QtQuick.Controls.Basic as Basic
import "../theme"

// The icon button of headers and rows, round like the cards it sits on. Files that import "../components" after QtQuick.Controls get this one
// wherever they write ToolButton.
Basic.ToolButton {
    id: button

    background: Rectangle {
        implicitWidth: Theme.toggleSize + 8
        implicitHeight: Theme.toggleSize + 8
        radius: Math.min(width, height) / 2
        color: button.down ? Theme.tint(Theme.text, Theme.selected)
             : button.highlighted ? Theme.tint(Theme.accent, 0.25)
             : button.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
        Behavior on color { ColorAnimation { duration: 90 } }
    }
    icon.color: Theme.text
    opacity: enabled ? 1 : 0.4
}
