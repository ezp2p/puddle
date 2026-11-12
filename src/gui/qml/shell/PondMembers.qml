import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The panel at the right of a Pond: who is in it, by the name each chose to show there.
Pane {
    id: panel

    required property var ponds
    property bool closable: false
    signal closeRequested()

    padding: 0
    background: null

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                Label { text: qsTr("Members — %1").arg(panel.ponds.members.count); font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                ToolButton {
                    visible: panel.closable
                    icon.source: Theme.icon("x"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Close")
                    onClicked: panel.closeRequested()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }
        ScrollList {
            objectName: "pondMemberList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 8
            model: panel.ponds.members
            delegate: RowLayout {
                required property var model
                width: ListView.view.width
                height: 44
                spacing: 10
                Avatar { Layout.leftMargin: 8; name: parent.model.name; tint: parent.model.color || ""; size: 32 }
                Label {
                    Layout.fillWidth: !badge.visible
                    elide: Text.ElideRight
                    text: parent.model.isMe ? qsTr("%1 (you)").arg(parent.model.name) : parent.model.name
                    color: parent.model.color ? parent.model.color : palette.text
                }
                RoleBadge { id: badge; role: parent.model.role || ""; Layout.rightMargin: 8 }
                Item { visible: badge.visible; Layout.fillWidth: true }
            }
        }
    }
}
