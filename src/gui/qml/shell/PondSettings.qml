import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Server Settings: what an operator or administrator runs from the app (the Pond's docs/design.md, "Roles"), over the window as
// Discord's are. On the left the sections, on the right the open one's page, in groups. Each change goes to the Pond through our
// Puddle; what it says back shows under the page's title. Escape or the cross closes it.
Popup {
    id: settings

    objectName: "pondSettings"
    required property var chats
    readonly property var ponds: chats.ponds
    property string section: "overview"

    // what happened to the last change: shown under the title until the next
    property string status: ""
    property bool statusBad: false
    function say(text, bad) { status = text; statusBad = bad; statusTimer.restart() }
    Timer { id: statusTimer; interval: 5000; onTriggered: settings.status = "" }
    Connections {
        target: settings.ponds
        function onAdminDone(what) { if (settings.opened) settings.say(what, false) }
        function onAdminFailed(why) { if (settings.opened) settings.say(why, true) }
        // the role went (taken away, or we left): nothing here is ours to change any more
        function onCurrentInfoChanged() { if (settings.opened && !settings.ponds.canAdmin) settings.close() }
    }

    function show(which) {
        section = which || "overview"
        status = ""
        open()
    }
    onSectionChanged: if (section === "bans") ponds.loadBans()
    onOpened: {
        overview.reset()
        if (section === "bans") ponds.loadBans()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(980, parent ? parent.width - 48 : 980)
    height: Math.min(700, parent ? parent.height - 48 : 700)
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 } }
    background: Rectangle { radius: Theme.cardRadius; color: Theme.card; border.width: 1; border.color: Theme.hairline }

    readonly property var sections: [
        { id: "overview", title: qsTr("Overview"), glyph: "settings" },
        { id: "channels", title: qsTr("Channels"), glyph: "hash" },
        { id: "members", title: qsTr("Members"), glyph: "users" },
        { id: "bans", title: qsTr("Bans"), glyph: "x" }
    ]
    function titleOf(id) { for (const s of sections) if (s.id === id) return s.title; return "" }

    contentItem: RowLayout {
        spacing: 0

        // ---- the sections ----
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: Theme.canvas
            radius: Theme.cardRadius
            // square off the corners that meet the page
            Rectangle { anchors.right: parent.right; width: Theme.cardRadius; height: parent.height; color: parent.color }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                anchors.topMargin: 24
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    Layout.bottomMargin: 6
                    text: settings.ponds.currentName.toUpperCase()
                    font.pixelSize: 11
                    font.bold: true
                    opacity: 0.55
                    elide: Text.ElideRight
                }
                Repeater {
                    model: settings.sections
                    delegate: Rectangle {
                        id: tab
                        required property var modelData
                        readonly property bool selected: settings.section === modelData.id
                        objectName: "settingsSection_" + modelData.id
                        Layout.fillWidth: true
                        height: 34
                        radius: Theme.radius
                        color: selected ? Theme.tint(Theme.text, Theme.selected) : tabHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: settings.section = tab.modelData.id }
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.title
                        Accessible.onPressAction: settings.section = tab.modelData.id
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 8
                            Icon { glyph: tab.modelData.glyph; size: 16; opacity: tab.selected ? 1 : 0.7 }
                            Label { text: tab.modelData.title; Layout.fillWidth: true; font.bold: tab.selected; opacity: tab.selected ? 1 : 0.85 }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.55
                    text: settings.ponds.currentRole === "operator" ? qsTr("You are an operator here.") : qsTr("You are an administrator here.")
                }
            }
        }

        // ---- the open section ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 32
                Layout.rightMargin: 16
                Layout.topMargin: 20
                Label { text: settings.titleOf(settings.section); font.bold: true; font.pixelSize: 20; Layout.fillWidth: true }
                ToolButton {
                    objectName: "closeSettings"
                    icon.source: Theme.icon("x"); icon.width: 18; icon.height: 18
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Close")
                    ToolTip.text: qsTr("Close (Esc)"); ToolTip.visible: hovered
                    onClicked: settings.close()
                }
            }
            Label {
                objectName: "settingsStatus"
                Layout.fillWidth: true
                Layout.leftMargin: 32
                Layout.rightMargin: 32
                Layout.minimumHeight: 20
                wrapMode: Text.Wrap
                text: settings.status
                color: settings.statusBad ? Theme.danger : Theme.primary
            }

            ScrollArea {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: pages.implicitHeight + 40
                StackLayout {
                    id: pages
                    x: 32
                    y: 8
                    width: scroller.width - 64
                    currentIndex: ["overview", "channels", "members", "bans"].indexOf(settings.section)

                    // ---- Overview ----
                    ColumnLayout {
                        id: overview
                        spacing: 16
                        function reset() { nameField.text = settings.ponds.currentName; descField.text = settings.ponds.currentDescription }
                        readonly property bool changed: nameField.text.trim() !== settings.ponds.currentName || descField.text.trim() !== settings.ponds.currentDescription
                        SettingsGroup {
                            title: qsTr("Pond")
                            Layout.fillWidth: true
                            Label { text: qsTr("Name"); font.bold: true; font.pixelSize: 12 }
                            TextField { id: nameField; objectName: "pondNameField"; Layout.fillWidth: true; maximumLength: 100 }
                            Label { text: qsTr("Description"); font.bold: true; font.pixelSize: 12; Layout.topMargin: 6 }
                            TextArea {
                                id: descField
                                objectName: "pondDescriptionField"
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(90, implicitHeight)
                                wrapMode: TextArea.Wrap
                                placeholderText: qsTr("What this Pond is for")
                                onTextChanged: if (length > 1000) remove(1000, length)
                                background: Rectangle { radius: Theme.buttonRadius; color: Theme.card; border.width: descField.activeFocus ? 2 : 1; border.color: descField.activeFocus ? Theme.accent : Theme.hairline }
                            }
                            RowLayout {
                                Layout.topMargin: 6
                                spacing: 8
                                PrimaryButton {
                                    objectName: "saveOverview"
                                    text: qsTr("Save changes")
                                    enabled: overview.changed && nameField.text.trim() !== ""
                                    onClicked: settings.ponds.saveOverview(nameField.text, descField.text)
                                }
                                Button { text: qsTr("Reset"); enabled: overview.changed; onClicked: overview.reset() }
                            }
                        }
                    }

                    // ---- Channels ----
                    ColumnLayout {
                        spacing: 16
                        SettingsGroup {
                            title: qsTr("Create a channel")
                            Layout.fillWidth: true
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                TextField {
                                    id: newName
                                    objectName: "newChannelName"
                                    Layout.preferredWidth: 220
                                    placeholderText: qsTr("name")
                                    maximumLength: 32
                                    // a channel name is lower case letters, digits and dashes: spaces become dashes as they are typed
                                    onTextEdited: { const t = text.toLowerCase().replace(/\s/g, "-").replace(/[^a-z0-9-]/g, ""); if (t !== text) text = t }
                                }
                                TextField { id: newTopic; Layout.fillWidth: true; placeholderText: qsTr("topic (optional)"); maximumLength: 1024 }
                                PrimaryButton {
                                    objectName: "createChannel"
                                    text: qsTr("Create")
                                    enabled: newName.text !== ""
                                    onClicked: { settings.ponds.createChannel(newName.text, newTopic.text); newName.text = ""; newTopic.text = "" }
                                }
                            }
                        }
                        SettingsGroup {
                            title: qsTr("Channels")
                            Layout.fillWidth: true
                            Repeater {
                                model: settings.ponds.channels
                                delegate: ColumnLayout {
                                    id: chRow
                                    required property var model
                                    property bool editing: false
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        visible: !chRow.editing
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Icon { glyph: "hash"; size: 16; opacity: 0.6 }
                                        Label { text: chRow.model.name; font.bold: true }
                                        Label { text: chRow.model.topic; opacity: 0.6; elide: Text.ElideRight; Layout.fillWidth: true }
                                        ToolButton {
                                            icon.source: Theme.icon("pencil"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Edit #%1").arg(chRow.model.name)
                                            ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered
                                            onClicked: { editName.text = chRow.model.name; editTopic.text = chRow.model.topic; chRow.editing = true }
                                        }
                                        ToolButton {
                                            icon.source: Theme.icon("trash"); icon.width: 16; icon.height: 16; icon.color: Theme.danger
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Delete #%1").arg(chRow.model.name)
                                            ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered
                                            onClicked: confirm.ask(qsTr("Delete #%1?").arg(chRow.model.name),
                                                                   qsTr("Nobody will be able to read it any more. This can't be undone from the app."),
                                                                   qsTr("Delete channel"), () => settings.ponds.deleteChannel(chRow.model.id))
                                        }
                                    }
                                    RowLayout {
                                        visible: chRow.editing
                                        Layout.fillWidth: true
                                        spacing: 8
                                        TextField {
                                            id: editName
                                            Layout.preferredWidth: 200
                                            maximumLength: 32
                                            onTextEdited: { const t = text.toLowerCase().replace(/\s/g, "-").replace(/[^a-z0-9-]/g, ""); if (t !== text) text = t }
                                        }
                                        TextField { id: editTopic; Layout.fillWidth: true; placeholderText: qsTr("topic"); maximumLength: 1024 }
                                        PrimaryButton {
                                            text: qsTr("Save")
                                            enabled: editName.text !== ""
                                            onClicked: { settings.ponds.editChannel(chRow.model.id, editName.text, editTopic.text); chRow.editing = false }
                                        }
                                        Button { text: qsTr("Cancel"); onClicked: chRow.editing = false }
                                    }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
                                }
                            }
                        }
                    }

                    // ---- Members ----
                    ColumnLayout {
                        spacing: 16
                        SettingsGroup {
                            title: qsTr("Members (%1)").arg(settings.ponds.members.count)
                            note: qsTr("Removing someone lets them back with a new invite; banning them does not, until they are unbanned. Operators are given and taken away by the server's admin (pondctl).")
                            Layout.fillWidth: true
                            Repeater {
                                model: settings.ponds.members
                                delegate: ColumnLayout {
                                    id: mRow
                                    required property var model
                                    readonly property bool canAct: settings.opened && settings.ponds.canActOn(model.id)
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        Avatar { name: mRow.model.name; tint: mRow.model.color || ""; size: 32 }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            RowLayout {
                                                spacing: 6
                                                Label { text: mRow.model.name + (mRow.model.isMe ? qsTr(" (you)") : ""); font.bold: true }
                                                RoleBadge { role: mRow.model.role }
                                            }
                                            Label { text: mRow.model.id; font.pixelSize: 11; opacity: 0.5; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                        }
                                        Button {
                                            text: qsTr("Remove")
                                            enabled: mRow.canAct
                                            onClicked: confirm.ask(qsTr("Remove %1?").arg(mRow.model.name),
                                                                   qsTr("They leave the Pond now, and can come back with a new invite."),
                                                                   qsTr("Remove"), () => settings.ponds.kick(mRow.model.id))
                                        }
                                        Button {
                                            text: qsTr("Ban")
                                            enabled: mRow.canAct
                                            palette.buttonText: Theme.danger
                                            onClicked: banDialog.ask(mRow.model.id, mRow.model.name)
                                        }
                                    }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
                                }
                            }
                        }
                    }

                    // ---- Bans ----
                    ColumnLayout {
                        spacing: 16
                        SettingsGroup {
                            title: qsTr("Banned (%1)").arg(settings.ponds.bans.count)
                            note: qsTr("Someone banned can't join again, even with an invite, until they are unbanned.")
                            Layout.fillWidth: true
                            Label { visible: settings.ponds.bans.count === 0; text: qsTr("Nobody is banned."); opacity: 0.6 }
                            Repeater {
                                model: settings.ponds.bans
                                delegate: ColumnLayout {
                                    id: bRow
                                    required property var model
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        ColumnLayout {
                                            spacing: 2
                                            Layout.fillWidth: true
                                            Label { text: bRow.model.name; font.bold: true }
                                            Label {
                                                Layout.fillWidth: true
                                                wrapMode: Text.Wrap
                                                font.pixelSize: 12
                                                opacity: 0.65
                                                text: qsTr("By %1, %2").arg(bRow.model.byName).arg(new Date(bRow.model.ts).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                                                      + (bRow.model.reason ? " · " + bRow.model.reason : "")
                                            }
                                        }
                                        Button { text: qsTr("Unban"); onClicked: settings.ponds.unban(bRow.model.member) }
                                    }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ConfirmDialog { id: confirm }

    // banning asks why (optional), which the other operators and administrators see in Bans
    Dialog {
        id: banDialog
        property string member: ""
        property string name: ""
        function ask(id, who) { member = id; name = who; reason.text = ""; open(); reason.forceActiveFocus() }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(400, parent ? parent.width - 24 : 400)
        title: qsTr("Ban %1?").arg(name)
        ColumnLayout {
            width: banDialog.availableWidth
            spacing: 8
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("They leave the Pond now, and can't join again until they are unbanned.") }
            TextField { id: reason; objectName: "banReason"; Layout.fillWidth: true; placeholderText: qsTr("Reason (optional)"); maximumLength: 500 }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { objectName: "confirmBan"; text: qsTr("Ban"); palette.buttonText: Theme.danger; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
        onAccepted: settings.ponds.ban(member, reason.text)
    }
}
