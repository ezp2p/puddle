import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The panel at the right of an open conversation. For a group: who is in it, ask more friends, rename it, leave. For a person: who they are and
// what can be done about them. Used beside the conversation when the window is wide, and as a drawer over it when it is not.
Rectangle {
    id: bar

    objectName: "contextBar"
    required property var chats
    property bool developer: false
    property bool closable: false     // in a drawer: show a button to close it
    signal closeRequested()
    signal openChat(string userId)    // a member (a friend) was chosen
    signal addPeople()                // ask more friends into the open group
    signal renameGroup()
    signal leaveGroup()

    readonly property bool isGroup: chats.currentIsGroup
    // the person's profile as last heard; looked at again when anything about the chat changes
    readonly property var info: { chats.currentName; chats.currentState; chats.people.count; return chats.person(chats.current) }

    color: "transparent"   // it is drawn on a card (or in a drawer)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                Label { text: bar.isGroup ? qsTr("Members") : qsTr("Profile"); font.bold: true; font.pixelSize: 15; Layout.fillWidth: true }
                ToolButton {
                    objectName: "contextClose"
                    visible: bar.closable
                    icon.source: Theme.icon("x"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Close")
                    onClicked: bar.closeRequested()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        // ---- a group ----
        ColumnLayout {
            visible: bar.isGroup
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 12
                Avatar { chats: bar.chats; name: bar.chats.currentName; size: 48 }
                Label { objectName: "groupName"; text: bar.chats.currentName; font.bold: true; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                ToolButton {
                    objectName: "groupRenameButton"
                    icon.source: Theme.icon("pencil"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    enabled: bar.chats.currentState === "active"
                    Accessible.name: qsTr("Rename group")
                    ToolTip.text: qsTr("Rename group"); ToolTip.visible: hovered
                    onClicked: bar.renameGroup()
                }
            }

            Label { Layout.leftMargin: 16; Layout.bottomMargin: 4; text: qsTr("%1 in this group").arg(bar.chats.members.count); font.pixelSize: 11; font.bold: true; opacity: 0.6 }

            ScrollList {
                id: members
                objectName: "memberList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                model: bar.chats.members
                delegate: Item {
                    id: mrow
                    required property var model
                    readonly property bool me: model.isMe === true
                    width: ListView.view.width
                    height: 44
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 12
                        Avatar { chats: bar.chats; name: mrow.model.name; hash: mrow.model.avatar || ""; tint: mrow.model.color || ""; size: 32 }
                        Label { text: mrow.model.name; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { visible: mrow.model.state === "invited"; text: qsTr("invited"); opacity: 0.6; font.pixelSize: 12 }
                    }
                    HoverHandler { id: mhover }
                    ContextTrigger { active: !mrow.me || bar.developer; onRequested: (x, y) => memberMenu.popup(mrow, x, y) }
                    ToolButton {
                        objectName: "memberMore"
                        visible: !mrow.me || bar.developer
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: mhover.hovered || hovered ? 1 : 0
                        enabled: opacity > 0
                        icon.source: Theme.icon("dots"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                        display: AbstractButton.IconOnly
                        Accessible.name: qsTr("More")
                        onClicked: memberMenu.popup(this, 0, height)
                    }
                    Menu {
                        id: memberMenu
                        MenuItem {
                            objectName: "memberMessage"; text: qsTr("Message")
                            visible: !mrow.me && mrow.model.friend === true; height: visible ? implicitHeight : 0
                            onTriggered: bar.openChat(mrow.model.id)
                        }
                        MenuItem {
                            objectName: "memberCopyId"; text: qsTr("Copy ID")
                            visible: bar.developer; height: visible ? implicitHeight : 0
                            onTriggered: bar.chats.copy(mrow.model.id)
                        }
                        MenuSeparator { visible: !mrow.me; height: visible ? implicitHeight : 0 }
                        MenuItem {
                            objectName: "memberBlock"; text: qsTr("Block…"); palette.text: Theme.danger
                            visible: !mrow.me; height: visible ? implicitHeight : 0
                            onTriggered: confirm.ask(qsTr("Block"),
                                                     qsTr("Block %1? They are cut off at once and cannot ask again until you unblock them.").arg(mrow.model.name),
                                                     qsTr("Block"), () => bar.chats.block(mrow.model.id))
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 8
                Button {
                    objectName: "addPeopleButton"
                    Layout.fillWidth: true
                    text: qsTr("Add people")
                    icon.source: Theme.icon("user-plus"); icon.width: 18; icon.height: 18
                    enabled: bar.chats.currentState === "active"
                    onClicked: bar.addPeople()
                }
                Button {
                    id: leave
                    objectName: "leaveGroupButton"
                    Layout.fillWidth: true
                    text: qsTr("Leave group…")
                    palette.buttonText: Theme.danger
                    onClicked: confirm.ask(qsTr("Leave group"),
                                           qsTr("Leave “%1”? The others will be told. You can only come back if someone invites you again.").arg(bar.chats.currentName),
                                           qsTr("Leave"), () => bar.chats.leaveGroup())
                }
            }
        }

        // ---- a person ----
        ScrollArea {
            id: personArea
            visible: !bar.isGroup
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: person.implicitHeight + 32

            ColumnLayout {
                id: person
                x: 16
                y: 16
                width: personArea.width - 32
                spacing: 10

                Avatar { chats: bar.chats; name: bar.chats.currentName; hash: bar.chats.currentAvatar; tint: bar.chats.currentColor; size: 72; Layout.alignment: Qt.AlignHCenter }
                Label {
                    objectName: "profileName"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.bold: true
                    font.pixelSize: 18
                    text: bar.chats.currentName
                }
                Label {
                    objectName: "profilePronouns"
                    visible: text !== ""
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    opacity: 0.65
                    text: bar.info.pronouns || ""
                }
                Label {
                    objectName: "profileBlurb"
                    visible: text !== ""
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: bar.info.blurb || ""
                }
                ColumnLayout {
                    visible: (bar.info.about || "") !== ""
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: qsTr("About"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                    Label { objectName: "profileAbout"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: bar.info.about || "" }
                }
                ColumnLayout {
                    visible: !!bar.info.since
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: bar.chats.currentState === "active" ? qsTr("Friends since") : qsTr("Since"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                    Label { objectName: "profileSince"; text: bar.info.since ? Qt.formatDate(new Date(bar.info.since), Locale.LongFormat) : "" }
                }
                Label {
                    visible: bar.chats.currentState === "closed"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.warning
                    text: qsTr("The connection with %1 is closed. You can read what was said, not write.").arg(bar.chats.currentName)
                }
                // Developer Mode: who they claim to be, and the code to compare with them
                ColumnLayout {
                    visible: bar.developer
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: qsTr("ID"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                    Label { objectName: "profileId"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 12; text: bar.chats.grouped(bar.chats.current) }
                    Label { text: qsTr("Code"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                    Label { objectName: "profileCode"; font.pixelSize: 12; text: bar.chats.shortCode(bar.chats.current) }
                    Button { text: qsTr("Copy ID"); onClicked: bar.chats.copy(bar.chats.current) }
                }
                Item { Layout.preferredHeight: 8 }
                Button {
                    objectName: "profileRemove"
                    Layout.fillWidth: true
                    visible: bar.chats.currentState === "active"
                    text: qsTr("Remove friend…")
                    palette.buttonText: Theme.danger
                    onClicked: confirm.ask(qsTr("Remove friend"),
                                           qsTr("Remove %1 from your friends? The connection between you is closed and they are told. Your messages stay.").arg(bar.chats.currentName),
                                           qsTr("Remove"), () => bar.chats.cancel(bar.chats.current))
                }
                Button {
                    objectName: "profileBlock"
                    Layout.fillWidth: true
                    text: qsTr("Block…")
                    palette.buttonText: Theme.danger
                    onClicked: confirm.ask(qsTr("Block"),
                                           qsTr("Block %1? They are cut off at once and cannot ask again until you unblock them.").arg(bar.chats.currentName),
                                           qsTr("Block"), () => bar.chats.block(bar.chats.current))
                }
            }
        }
    }

    ConfirmDialog { id: confirm; objectName: "contextConfirm" }
}
