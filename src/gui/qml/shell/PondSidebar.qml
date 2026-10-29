import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The left-hand list while a Pond is open: its name (with a menu to choose how you appear there, or leave), then its text channels.
Rectangle {
    id: bar

    objectName: "pondSidebar"
    color: "transparent"   // it is drawn on a card
    required property var chats
    readonly property var ponds: chats.ponds
    signal leaveRequested()

    // Alt+Up and Alt+Down walk through the channels
    function step(delta) {
        const n = list.count
        if (n === 0) return
        let i = list.currentIndex < 0 ? (delta > 0 ? 0 : n - 1) : Math.max(0, Math.min(n - 1, list.currentIndex + delta))
        list.currentIndex = i
        list.currentItem.openIt()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: nameHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
            HoverHandler { id: nameHover }
            TapHandler { onTapped: menu.popup() }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                Label { objectName: "pondName"; text: bar.ponds.currentName; font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
                Icon { glyph: "chevron-down"; size: 16; opacity: 0.6 }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
            Menu {
                id: menu
                y: parent.height
                Menu {
                    title: qsTr("Appear as")
                    Repeater {
                        model: bar.chats.profiles
                        MenuItem { required property var model; text: model.display_name; onTriggered: bar.ponds.setProfile(bar.ponds.current, model.id) }
                    }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Nobody (only your id)"); onTriggered: bar.ponds.setProfile(bar.ponds.current, "") }
                }
                MenuSeparator {}
                MenuItem { objectName: "leavePond"; text: qsTr("Leave Pond…"); palette.text: Theme.danger; onTriggered: bar.leaveRequested() }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.topMargin: 14
            Layout.bottomMargin: 4
            text: qsTr("TEXT CHANNELS")
            font.pixelSize: 11
            font.bold: true
            opacity: 0.55
        }

        ScrollList {
            id: list
            objectName: "channelList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            model: bar.ponds.channels
            currentIndex: -1
            delegate: Rectangle {
                id: row
                required property var model
                required property int index
                readonly property bool selected: model.id === bar.ponds.channel
                function openIt() { bar.ponds.openChannel(model.id) }
                objectName: "channel_" + model.name
                width: ListView.view.width
                height: 34
                radius: Theme.radius
                color: selected ? Theme.tint(Theme.text, Theme.selected) : rowHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                HoverHandler { id: rowHover }
                TapHandler { onTapped: row.openIt() }
                Accessible.role: Accessible.Button
                Accessible.name: "#" + model.name
                Accessible.onPressAction: row.openIt()
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 6
                    Icon { glyph: "hash"; size: 18; opacity: row.selected ? 1 : 0.6 }
                    Label { text: row.model.name; Layout.fillWidth: true; elide: Text.ElideRight; opacity: row.selected ? 1 : 0.8; font.bold: row.selected }
                }
                ToolTip.text: row.model.topic
                ToolTip.visible: rowHover.hovered && row.model.topic !== ""
                ToolTip.delay: 600
            }
        }

        Label {
            visible: bar.ponds.channels.count === 0
            Layout.fillWidth: true
            Layout.margins: 16
            wrapMode: Text.Wrap
            opacity: 0.6
            text: bar.ponds.currentStatus === "connecting" ? qsTr("Connecting…") : qsTr("This Pond has no channels yet.")
        }
    }
}
