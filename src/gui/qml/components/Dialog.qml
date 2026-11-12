import QtQuick
import QtQuick.Controls.Basic as Basic
import "../theme"

// A dialog on a card: rounded, with a hairline, and the buttons of DialogButtonBox.qml. Files that import "../components" after QtQuick.Controls
// get this one wherever they write Dialog.
Basic.Dialog {
    id: dialog

    background: Rectangle {
        color: Theme.card
        radius: Theme.cardRadius - 6
        border.width: 1
        border.color: Theme.hairline
    }
    footer: DialogButtonBox { }
}
