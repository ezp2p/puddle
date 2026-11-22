import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// One of a page's media in its editor: the picture (or a file's icon), its name, Insert (into the markdown) and a cross to take it off.
Rectangle {
    id: tile
    property var chats
    property string name
    property bool image: false
    property string url: ""
    property bool fresh: false     // not sent yet: goes with the page when it is saved
    signal insert()
    signal remove()

    width: 156
    height: 172
    radius: Theme.radiusLarge
    color: Theme.tint(Theme.text, Theme.hover)
    border.width: 1
    border.color: fresh ? Theme.tint(Theme.accent, 0.5) : Theme.hairline
    clip: true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            radius: Theme.radius
            color: Theme.tint(Theme.text, Theme.selected)
            clip: true
            Image {
                anchors.fill: parent
                visible: tile.url !== ""
                source: tile.url
                fillMode: Image.PreserveAspectCrop
                sourceSize: Qt.size(280, 200)
                asynchronous: true
            }
            Icon { anchors.centerIn: parent; visible: tile.url === ""; glyph: tile.image ? "photo" : "file"; size: 28; opacity: 0.6 }
        }
        Label { Layout.fillWidth: true; text: tile.name; elide: Text.ElideMiddle; font.pixelSize: 12 }
        RowLayout {
            spacing: 4
            Button {
                objectName: "insertMedia"
                text: qsTr("Insert")
                Layout.fillWidth: true
                onClicked: tile.insert()
            }
            ToolButton {
                icon.source: Theme.icon("x"); icon.width: 14; icon.height: 14
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Take %1 off").arg(tile.name)
                ToolTip.text: Accessible.name; ToolTip.visible: hovered
                onClicked: tile.remove()
            }
        }
    }
}
