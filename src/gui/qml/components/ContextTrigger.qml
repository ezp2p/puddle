import QtQuick

// Asks for a context menu the ways people expect: a right-click (Ctrl+click on a Mac arrives as one), press and hold on a touch
// screen, and the Menu key or Shift+F10 while the parent has the keyboard focus. Put it inside the item the menu is about. It fills
// that item and takes no clicks of its own, so the item underneath still works as before.
Item {
    id: root

    anchors.fill: parent
    property bool active: true
    // where the menu was asked for, in the parent's coordinates
    signal requested(real x, real y)

    TapHandler {
        enabled: root.active
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => root.requested(eventPoint.position.x, eventPoint.position.y)
    }
    TapHandler {
        enabled: root.active
        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.TouchScreen
        longPressThreshold: 0.6
        onLongPressed: root.requested(point.position.x, point.position.y)
    }
    Shortcut {
        enabled: root.active && root.parent !== null && root.parent.activeFocus
        sequences: ["Menu", "Shift+F10"]
        onActivated: root.requested(Math.min(24, root.width), root.height)
    }
}
