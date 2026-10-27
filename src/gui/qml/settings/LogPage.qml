import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Settings, and what the Puddle has been doing, newest at the bottom.
Pane {
    id: page

    required property var backend
    required property var chats
    property bool narrow: false
    property bool sidebarShown: true
    signal toggleSidebar()

    padding: 0
    background: null

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            PanelToggle { glyph: "layout-sidebar"; label: qsTr("Conversations"); active: page.sidebarShown; onClicked: page.toggleSidebar() }
            Label { text: qsTr("Settings"); font.bold: true; font.pixelSize: 16 }
            Item { Layout.fillWidth: true }
            Label {
                text: page.backend.error === "" ? qsTr("Listening on %1").arg(page.backend.listenUrl) : qsTr("Not running")
                opacity: 0.6
                elide: Text.ElideMiddle
                Layout.maximumWidth: page.width * 0.6
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }

        // Like Discord's Developer Mode: what is normally kept out of sight
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label { text: qsTr("Developer Mode"); font.bold: true }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: qsTr("Show user IDs and codes, and let you copy them.")
                }
            }
            Switch {
                objectName: "developerSwitch"
                checked: page.backend.developerMode
                onToggled: page.backend.developerMode = checked
                Accessible.name: qsTr("Developer Mode")
            }
        }
        RowLayout {
            visible: page.backend.developerMode && page.backend.myId !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            Label { text: qsTr("Your user ID"); opacity: 0.65 }
            TextField {
                objectName: "myIdField"
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: page.backend.myId
                font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
                font.pixelSize: 12
                Accessible.name: qsTr("Your user ID")
                FieldMenu {}
            }
            Button { text: qsTr("Copy"); onClicked: page.backend.copyText(page.backend.myId) }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
        // people you blocked, and a way to let them back
        Label { visible: page.chats.blocked.count > 0; Layout.margins: 16; Layout.bottomMargin: 4; text: qsTr("Blocked"); font.bold: true }
        Repeater {
            model: page.chats.blocked
            delegate: PersonRow {
                required property var model
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                chats: page.chats
                mode: "blocked"
                developer: page.backend.developerMode
                userId: model.user
                name: model.name
            }
        }
        Rectangle { visible: page.chats.blocked.count > 0; Layout.fillWidth: true; height: 1; color: Theme.hairline; Layout.topMargin: 8 }
        Label { Layout.margins: 16; Layout.bottomMargin: 0; text: qsTr("Activity"); font.bold: true }
        Label {
            visible: page.backend.error !== ""
            Layout.fillWidth: true
            Layout.margins: 16
            wrapMode: Text.Wrap
            color: Theme.danger
            text: page.backend.error
        }
        ScrollList {
            id: logView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 12
            model: page.backend.log
            onCountChanged: positionViewAtEnd()
            delegate: Label {
                required property string display
                width: ListView.view.width
                text: display
                wrapMode: Text.WrapAnywhere
                font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
                font.pixelSize: 13
            }
        }
    }
}
