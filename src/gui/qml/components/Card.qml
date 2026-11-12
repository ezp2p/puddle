import QtQuick
import "../theme"

// A panel that floats on the window's canvas: rounded, a hairline around it. What goes inside keeps off the corners, so it needs no clipping.
Rectangle {
    color: Theme.card
    radius: Theme.cardRadius
    border.width: 1
    border.color: Theme.hairline
}
