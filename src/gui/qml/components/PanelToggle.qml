import QtQuick
import QtQuick.Controls
import "../theme"

// The little circle that folds a side panel away, or brings it back: PanelToggle { glyph: "layout-sidebar"; label: qsTr("Conversations") }.
Rectangle {
    id: toggle

    required property string glyph
    required property string label
    property bool active: false      // the panel it controls is showing
    signal clicked()

    implicitWidth: Theme.toggleSize
    implicitHeight: Theme.toggleSize
    radius: width / 2
    color: tap.pressed ? Theme.tint(Theme.text, Theme.selected) : hover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
    border.width: 1
    border.color: Theme.hairline
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.checkable: true
    Accessible.checked: active
    Accessible.onPressAction: toggle.clicked()
    Keys.onSpacePressed: toggle.clicked()
    Keys.onReturnPressed: toggle.clicked()

    Icon { anchors.centerIn: parent; glyph: toggle.glyph; size: 16; color: Theme.text; opacity: toggle.active ? 1 : 0.6 }
    // keyboard focus ring
    Rectangle { anchors.fill: parent; anchors.margins: -3; radius: width / 2; color: "transparent"; border.width: 2; border.color: Theme.accent; visible: toggle.activeFocus }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: toggle.clicked() }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: label
}
