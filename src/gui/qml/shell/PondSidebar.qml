import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The left-hand list while a Pond is open: its name, which drops down the Pond's menu (Server Settings for an operator or
// administrator, how you appear there, your id, leaving), a role the Pond offers you if it does, then its text channels.
Rectangle {
    id: bar

    objectName: "pondSidebar"
    color: "transparent"   // it is drawn on a card
    required property var chats
    readonly property var ponds: chats.ponds
    signal leaveRequested()
    signal settingsRequested()
    signal notice(string text)

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

        // the name: a click drops the Pond's menu down from it, as Discord's server menu does
        Rectangle {
            id: header
            objectName: "pondHeader"
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: nameHover.hovered || dropdown.opened ? Theme.tint(Theme.text, Theme.hover) : "transparent"
            HoverHandler { id: nameHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: dropdown.opened ? dropdown.close() : dropdown.open() }
            Accessible.role: Accessible.ButtonMenu
            Accessible.name: qsTr("%1 menu").arg(bar.ponds.currentName)
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                Label { objectName: "pondName"; text: bar.ponds.currentName; font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
                Icon { glyph: dropdown.opened ? "x" : "chevron-down"; size: 16; opacity: 0.7 }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }

            Popup {
                id: dropdown
                objectName: "pondDropdown"
                x: 8
                y: parent.height + 4
                width: parent.width - 16
                padding: 6
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                background: Rectangle {
                    radius: Theme.radiusLarge
                    color: Theme.card
                    border.width: 1
                    border.color: Theme.hairline
                }
                enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
                exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }

                contentItem: Column {
                    spacing: 2
                    PondMenuRow {
                        objectName: "serverSettings"
                        visible: bar.ponds.canAdmin
                        label: qsTr("Server Settings")
                        glyph: "settings"
                        onActivated: { dropdown.close(); bar.settingsRequested() }
                    }
                    PondMenuRow {
                        objectName: "appearAsItem"
                        label: qsTr("Appear as…")
                        glyph: "users"
                        onActivated: { dropdown.close(); appearMenu.popup(header, 8, header.height + 4) }
                    }
                    PondMenuRow {
                        objectName: "copyMyId"
                        label: qsTr("Copy my id")
                        glyph: "link"
                        onActivated: { dropdown.close(); bar.chats.copy(bar.ponds.myId); bar.notice(qsTr("Your id is copied. The Pond's admin uses it to give you a role.")) }
                    }
                    Rectangle { width: parent.width; height: 9; color: "transparent"; Rectangle { anchors.centerIn: parent; width: parent.width - 12; height: 1; color: Theme.hairline } }
                    PondMenuRow {
                        objectName: "leavePond"
                        label: qsTr("Leave Pond")
                        glyph: "x"
                        danger: true
                        onActivated: { dropdown.close(); bar.leaveRequested() }
                    }
                }
            }
            Menu {
                id: appearMenu
                Repeater {
                    model: bar.chats.profiles
                    MenuItem { required property var model; text: model.display_name; onTriggered: bar.ponds.setProfile(bar.ponds.current, model.id) }
                }
                MenuSeparator {}
                MenuItem { text: qsTr("Nobody (only your id)"); onTriggered: bar.ponds.setProfile(bar.ponds.current, "") }
            }
        }

        // a role the Pond offers us: ours to take or leave
        Rectangle {
            objectName: "roleOffer"
            visible: bar.ponds.currentOffer !== ""
            Layout.fillWidth: true
            Layout.margins: 8
            implicitHeight: offerCol.implicitHeight + 20
            radius: Theme.radiusLarge
            color: Theme.tint(Theme.accent, 0.14)
            border.width: 1
            border.color: Theme.tint(Theme.accent, 0.5)
            ColumnLayout {
                id: offerCol
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.bold: true
                    text: bar.ponds.currentOffer === "operator" ? qsTr("%1 invites you to be an operator").arg(bar.ponds.currentName)
                                                               : qsTr("%1 invites you to be an administrator").arg(bar.ponds.currentName)
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.75
                    font.pixelSize: 12
                    text: bar.ponds.currentOffer === "operator" ? qsTr("Operators run the Pond: its channels, its members, and its administrators.")
                                                               : qsTr("Administrators look after the Pond: its channels and its members.")
                }
                RowLayout {
                    spacing: 8
                    PrimaryButton { objectName: "acceptOffer"; text: qsTr("Accept"); onClicked: bar.ponds.answerOffer(true) }
                    Button { objectName: "declineOffer"; text: qsTr("Decline"); onClicked: bar.ponds.answerOffer(false) }
                }
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
