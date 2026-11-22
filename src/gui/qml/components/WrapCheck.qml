import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A check box whose words wrap, so a narrow column never has to grow to fit them.
CheckBox {
    id: c
    Layout.fillWidth: true
    contentItem: Label {
        leftPadding: c.indicator.width + c.spacing
        text: c.text
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
        opacity: c.enabled ? 1 : 0.5
    }
}
