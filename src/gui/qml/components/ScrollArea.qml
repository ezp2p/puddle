import QtQuick
import QtQuick.Controls

// The same for a page of items that is taller than the window: a Flickable that scrolls with the wheel, trackpad and scroll bar, not by
// dragging. Set `contentHeight` as for any Flickable.
Flickable {
    id: area

    signal userScrolled()

    clip: true
    interactive: false
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { onPositionChanged: if (pressed) area.userScrolled() }
    WheelScroll { view: area; onScrolled: area.userScrolled() }
}
