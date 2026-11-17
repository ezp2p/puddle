import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl
import "../theme"

// A text button with soft, rounded corners and a hairline, to match the cards. Files that import "../components" after QtQuick.Controls get
// this one wherever they write Button; the green action buttons give themselves a background of their own.
Basic.Button {
    id: button

    background: Rectangle {
        implicitWidth: 80
        implicitHeight: 36
        radius: Theme.buttonRadius
        color: button.down ? Theme.tint(Theme.text, Theme.selected)
             : button.highlighted ? Theme.tint(Theme.accent, 0.25)
             : button.hovered ? Theme.tint(Theme.text, Theme.selected * 0.8) : Theme.tint(Theme.text, Theme.hover)
        border.width: 1
        border.color: button.visualFocus ? Theme.accent : Theme.hairline
        Behavior on color { ColorAnimation { duration: 90 } }
    }
    contentItem: IconLabel {
        spacing: button.spacing
        mirrored: button.mirrored
        display: button.display
        icon: button.icon
        text: button.text
        font: button.font
        color: button.palette.buttonText
    }
    opacity: enabled ? 1 : 0.4
}
