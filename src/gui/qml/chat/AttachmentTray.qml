import QtQuick
import QtQuick.Controls
import "../components"
import "../theme"

// The files that go with the next message (a Composer's items): a tile each, with its upload once it is going, and a button
// to take it off. Shared by conversations and Pond channels.
Flow {
    id: tray

    required property var composer

    objectName: "attachments"
    visible: composer.count > 0
    spacing: 8
    Repeater {
        model: tray.composer.items
        delegate: Rectangle {
            id: tile
            required property var model
            objectName: "attachment"
            width: 76; height: 76
            radius: Theme.radius
            color: Theme.tint(palette.text, 0.08)
            border.width: model.state === "failed" ? 2 : 0
            border.color: Theme.danger
            clip: true
            Accessible.name: model.name
            Image {
                anchors.fill: parent
                visible: status === Image.Ready
                source: tile.model.kind === "image" ? tile.model.url : tile.model.kind === "video" ? tile.model.thumb : ""
                sourceSize: Qt.size(152, 152)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Column {
                anchors.centerIn: parent
                width: parent.width - 8
                visible: tile.model.kind === "file" || (tile.model.kind === "video" && tile.model.thumb === "")
                spacing: 2
                Icon { anchors.horizontalCenter: parent.horizontalCenter; glyph: tile.model.kind === "video" ? "player-play" : "file"; size: 22 }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideMiddle; font.pixelSize: 10; text: tile.model.name }
            }
            Rectangle { // a video's play mark over its first frame
                visible: tile.model.kind === "video" && tile.model.thumb !== ""
                anchors.centerIn: parent
                width: 26; height: 26; radius: 13
                color: Qt.rgba(0, 0, 0, 0.55)
                Icon { anchors.centerIn: parent; glyph: "player-play"; size: 14; color: "white" }
            }
            Label { visible: tile.model.state === "preparing"; anchors.centerIn: parent; text: "…"; font.pixelSize: 18 }
            Rectangle { // how much has gone
                visible: tile.model.state === "uploading"
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: 4
                color: Theme.tint(palette.text, 0.2)
                Rectangle { width: parent.width * tile.model.progress; height: parent.height; color: Theme.primary }
            }
            ToolButton {
                objectName: "removeAttachment"
                visible: !tray.composer.sending
                anchors.top: parent.top; anchors.right: parent.right
                width: 24; height: 24
                icon.source: Theme.icon("x"); icon.width: 14; icon.height: 14
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Remove %1").arg(tile.model.name)
                background: Rectangle { radius: 12; color: Qt.rgba(0, 0, 0, 0.45) }
                icon.color: "white"
                onClicked: tray.composer.remove(tile.model.key)
            }
            ToolTip.text: tile.model.name
            ToolTip.visible: tileHover.hovered
            HoverHandler { id: tileHover }
        }
    }
}
