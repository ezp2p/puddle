import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// One line of the sidebar. A friend or a group: the name, the last thing said, and how many are unread. Or something waiting for an answer:
// an invitation to a group, or a friend request, with the buttons to say yes or no. Or a request of ours nobody has answered, greyed.
ItemDelegate {
    id: row

    objectName: "dmRow"
    required property var model
    required property var chats
    property bool selected: false

    signal menuRequested(Item from, real x, real y)
    signal joinRequested()   // an invitation was said yes to: the caller says what joining costs first

    readonly property string kind: model.kind
    readonly property bool pending: kind === "invite" || kind === "request"
    readonly property bool waiting: kind === "sent"
    readonly property string convName: model.name || ""
    readonly property int unread: model.unread || 0

    height: Theme.rowHeight
    highlighted: selected
    Accessible.name: convName + (unread > 0 ? qsTr(", %n unread", "", unread) : "") + (pending || waiting ? ", " + (model.preview || "") : "")

    background: Rectangle {
        radius: Theme.radius
        color: row.highlighted ? Theme.tint(palette.text, Theme.selected)
               : row.hovered ? Theme.tint(palette.text, Theme.hover)
               : row.pending ? Theme.tint(palette.highlight, 0.1) : "transparent"
    }

    ContextTrigger { onRequested: (x, y) => row.menuRequested(row, x, y) }

    contentItem: RowLayout {
        spacing: 12
        Avatar { chats: row.chats; name: row.convName; hash: row.model.avatar || ""; tint: row.model.color || ""; size: 32; opacity: row.waiting ? 0.6 : 1 }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                text: row.convName
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.bold: row.unread > 0 || row.pending
                opacity: row.waiting ? 0.6 : 1
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.pixelSize: 11
                opacity: 0.6
                text: row.model.preview || ""
                visible: text !== ""
            }
        }
        // something to answer: yes or no, here
        ToolButton {
            objectName: row.kind === "invite" ? "acceptGroupButton" : "acceptButton"
            visible: row.pending
            icon.source: Theme.icon("check"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; icon.color: Theme.primary
            display: AbstractButton.IconOnly
            Accessible.name: row.kind === "invite" ? qsTr("Join") : qsTr("Accept")
            ToolTip.text: Accessible.name; ToolTip.visible: hovered
            onClicked: row.kind === "invite" ? row.joinRequested() : row.chats.accept(row.model.id)
        }
        ToolButton {
            objectName: row.kind === "invite" ? "declineGroupButton" : "declineButton"
            visible: row.pending
            icon.source: Theme.icon("x"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Decline")
            ToolTip.text: Accessible.name; ToolTip.visible: hovered
            onClicked: row.kind === "invite" ? row.chats.declineGroup(row.model.id) : row.chats.decline(row.model.id)
        }
        Rectangle {
            visible: row.unread > 0 && !row.pending
            Layout.preferredWidth: Math.max(18, unreadLabel.implicitWidth + 8); Layout.preferredHeight: 18; radius: 9; color: Theme.badge
            Label { id: unreadLabel; anchors.centerIn: parent; text: row.unread; color: "white"; font.pixelSize: 10; font.bold: true }
        }
        // the same menu, for those who do not right-click
        ToolButton {
            objectName: "rowMore"
            visible: !row.pending
            Layout.preferredWidth: 28; Layout.preferredHeight: 28
            opacity: row.hovered || hovered || row.activeFocus ? 1 : 0
            enabled: opacity > 0
            icon.source: Theme.icon("dots"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("More")
            onClicked: row.menuRequested(this, 0, height)
        }
    }
}
