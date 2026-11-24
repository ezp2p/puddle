import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// A small set of emoji to react with (docs/ponds.md, "Replies, reactions and mentions"). Opened at a message; picking one says
// which, and closes it. The last few picked come first.
Popup {
    id: picker
    signal picked(string emoji)
    property var recent: []
    padding: 8
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var common: [
        "👍", "👎", "❤️", "😂", "😮", "😢", "😡", "🎉",
        "🙏", "👀", "🔥", "✅", "❌", "💯", "🤔", "😍",
        "😅", "🙌", "👏", "🤝", "💪", "⭐", "🚀", "🐸",
        "☕", "🍕", "🌧️", "☀️", "🎶", "💡", "📌", "⚠️"
    ]
    readonly property var shown: recent.concat(common.filter(e => recent.indexOf(e) < 0))

    background: Rectangle { color: Theme.card; border.color: Theme.hairline; radius: Theme.radius }

    contentItem: GridLayout {
        columns: 8
        columnSpacing: 2
        rowSpacing: 2
        Repeater {
            model: picker.shown
            delegate: ToolButton {
                required property string modelData
                objectName: "emojiChoice"
                text: modelData
                font.pixelSize: 20
                implicitWidth: 36; implicitHeight: 36
                Accessible.name: modelData
                onClicked: {
                    picker.recent = [modelData].concat(picker.recent.filter(e => e !== modelData)).slice(0, 8)
                    picker.picked(modelData)
                    picker.close()
                }
            }
        }
    }
}
