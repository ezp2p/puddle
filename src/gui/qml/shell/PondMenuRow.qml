import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// One line of a Pond's menu (PondSidebar): words on the left, a sign on the right, and the whole line lit under the pointer,
// as in Discord's server menu. `danger` is for leaving.
Rectangle {
    id: row

    property string label
    property string glyph
    property bool danger: false
    signal activated()

    width: parent ? parent.width : 0
    height: 34
    radius: Theme.radius
    color: tap.pressed ? Qt.darker(danger ? Theme.danger : Theme.accent, 1.15) : hover.hovered ? (danger ? Theme.danger : Theme.accent) : "transparent"
    readonly property color ink: hover.hovered ? "white" : danger ? Theme.danger : Theme.text

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: row.activated() }
    Accessible.role: Accessible.MenuItem
    Accessible.name: label
    Accessible.onPressAction: row.activated()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        Label { text: row.label; color: row.ink; font.pixelSize: 14; Layout.fillWidth: true; elide: Text.ElideRight }
        Icon { glyph: row.glyph; size: 16; color: row.ink; visible: row.glyph !== "" }
    }
}
