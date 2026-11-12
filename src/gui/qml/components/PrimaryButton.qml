import QtQuick
import "../theme"

// The main action of a page or dialog: the filled button, in Theme.primary.
Button {
    id: button

    font.bold: true
    palette.buttonText: "white"
    background: Rectangle {
        implicitWidth: 80
        implicitHeight: 36
        radius: Theme.buttonRadius
        color: button.down ? Theme.primaryPressed : button.hovered ? Qt.lighter(Theme.primary, 1.08) : Theme.primary
        border.width: button.visualFocus ? 2 : 0
        border.color: Theme.accent
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
