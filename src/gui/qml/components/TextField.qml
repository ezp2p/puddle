import QtQuick
import QtQuick.Controls.Basic as Basic
import "../theme"

// A one-line box with rounded corners and the same padding on every side. Files that import "../components" after QtQuick.Controls get this
// one wherever they write TextField.
Basic.TextField {
    id: field

    leftPadding: 12
    rightPadding: 12
    topPadding: 9
    bottomPadding: 9
    color: Theme.text
    placeholderTextColor: Theme.tint(Theme.text, 0.45)
    selectionColor: Theme.accent
    selectByMouse: true
    background: Rectangle {
        implicitWidth: 160
        implicitHeight: 38
        radius: Theme.buttonRadius
        color: Theme.card
        border.width: field.activeFocus ? 2 : 1
        border.color: field.activeFocus ? Theme.accent : Theme.hairline
    }
}
