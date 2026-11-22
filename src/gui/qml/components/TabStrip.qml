import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// Tabs as words, the chosen one underlined (as Discord's settings are): `tabs` is [{text, name (objectName), enabled}].
Item {
    id: strip
    property var tabs: []
    property int currentIndex: 0
    implicitHeight: 36
    Layout.fillWidth: true

    Row {
        id: row
        height: parent.height
        spacing: 20
        Repeater {
            model: strip.tabs
            delegate: Item {
                id: tab
                required property var modelData
                required property int index
                readonly property bool current: strip.currentIndex === index
                readonly property bool usable: modelData.enabled !== false
                objectName: modelData.name || ""
                width: label.implicitWidth
                height: row.height
                enabled: usable
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.text
                Label {
                    id: label
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.modelData.text
                    font.bold: tab.current
                    opacity: !tab.usable ? 0.35 : tab.current || hover.hovered ? 1 : 0.65
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 2
                    radius: 1
                    color: Theme.primary
                    visible: tab.current
                }
                HoverHandler { id: hover; cursorShape: tab.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: strip.currentIndex = tab.index }
            }
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline; z: -1 }
}
