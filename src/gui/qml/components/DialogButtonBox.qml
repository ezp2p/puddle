import QtQuick
import QtQuick.Controls.Basic as Basic
import "../theme"

// The row of buttons under a dialog (Ok, Cancel, ...), drawn with our Button rather than the style's square one.
Basic.DialogButtonBox {
    visible: count > 0
    spacing: 8
    padding: 12
    delegate: Button {}
    background: Item {}
}
