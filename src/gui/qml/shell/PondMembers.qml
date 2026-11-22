import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The panel at the right of a Pond: who is in it, by the name each chose to show there.
Pane {
    id: panel

    required property var ponds
    property var chats: null       // for the members' pictures (Chats.avatarFor)
    property var people: null      // the Shell: someone's card and menu (showCard, personMenuAt)
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
            // where a role is shown apart: a heading for each such role, then one for everyone else (the Pond's docs/design.md,
            // "Permissions")
            section.property: "group"
            section.criteria: ViewSection.FullString
            section.delegate: Item {
                required property string section
                objectName: "pondMemberGroup_" + section
                width: ListView.view ? ListView.view.width : 0
                height: section === "" ? 0 : 30
                visible: section !== ""
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: parent.section.toUpperCase()
                    font.pixelSize: 11
                    font.bold: true
                    opacity: 0.6
                }
            }
            delegate: Item {
                id: pRow
                required property var model
                objectName: "pondMember_" + model.name
                width: ListView.view.width
                height: 44
                // a click shows their card under their name; a right-click, what can be done about them
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: if (panel.people) panel.people.showCard(pName, pRow.model.id) }
                ContextTrigger { onRequested: (x, y) => { if (panel.people) panel.people.personMenuAt(pRow, x, y, pRow.model.id) } }
                RowLayout {
                    anchors.fill: parent
                    spacing: 10
                    Avatar { objectName: "pondMemberAvatar"; Layout.leftMargin: 8; chats: panel.chats; name: pRow.model.name; hash: pRow.model.avatar || ""; tint: pRow.model.color || ""; size: 32 }
                    Label {
                        id: pName
                        Layout.fillWidth: !badge.visible && !mutedMark.visible
                        elide: Text.ElideRight
                        text: pRow.model.isMe ? qsTr("%1 (you)").arg(pRow.model.name) : pRow.model.name
                        color: pRow.model.nameColor ? pRow.model.nameColor : palette.text // their role's colour if they chose it (or the Pond says), else their own
                    }
                    RoleBadge { id: badge; role: pRow.model.role || ""; Layout.rightMargin: mutedMark.visible ? 0 : 8 }
                    // muted by an operator or administrator
                    Icon { id: mutedMark; objectName: "pondMemberMuted"; visible: pRow.model.muted === true; glyph: "microphone-off"; size: 14; color: Theme.danger; Layout.rightMargin: 8 }
                    Item { visible: badge.visible || mutedMark.visible; Layout.fillWidth: true }
                }
            }
        }
    }
}
