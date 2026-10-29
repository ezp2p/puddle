import QtQuick
import QtQuick.Controls

// A list that scrolls the way a desktop list does: the wheel, the trackpad, the scroll bar and the keyboard, but not by dragging the
// content with the mouse. Use it wherever a ListView would be.
ListView {
    id: list

    // the person scrolled (wheel, trackpad or scroll bar), as opposed to the list moving because its contents changed
    signal userScrolled()

    clip: true
    interactive: false
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { onPositionChanged: if (pressed) list.userScrolled() }
    WheelScroll { view: list; onScrolled: list.userScrolled() }
}
