import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A radio button whose words wrap, so a narrow column never has to grow to fit them.
RadioButton {
    id: r
    Layout.fillWidth: true
    contentItem: Label {
        leftPadding: r.indicator.width + r.spacing
        text: r.text
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
        opacity: r.enabled ? 1 : 0.5
    }
}
