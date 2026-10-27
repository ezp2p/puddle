import QtQuick
import QtQuick.Controls.impl
import "../theme"

// A Tabler icon, drawn in one colour: Icon { glyph: "users" }. Buttons do not need this; they take `icon.source: Theme.icon("users")`.
IconImage {
    required property string glyph
    property int size: Theme.iconSize

    source: Theme.icon(glyph)
    sourceSize: Qt.size(size, size)
    width: size
    height: size
    color: palette.windowText
}
