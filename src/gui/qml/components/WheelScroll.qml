import QtQuick
import "../theme"

// Mouse-wheel and trackpad scrolling for a Flickable that is not meant to be dragged with the mouse (on a desktop, dragging a list to
// scroll it is unexpected, and it clashes with selecting text). Put it inside the Flickable, which must have `interactive: false`.
WheelHandler {
    id: root

    // the Flickable to scroll (a handler declared inside one is attached to its content, not to the Flickable itself, so it is named)
    required property Flickable view
    signal scrolled()

    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    onWheel: event => {
        // a trackpad gives pixels; a wheel gives notches (120 units each)
        const dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * Theme.wheelStep
        const top = view.originY
        const bottom = Math.max(top, view.originY + view.contentHeight - view.height)
        view.contentY = Math.max(top, Math.min(bottom, view.contentY - dy))
        root.scrolled()
        event.accepted = true
    }
}
