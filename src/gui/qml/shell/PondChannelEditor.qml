import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// One channel in Server Settings, on a page of its own (back to the list from its header), in tabs as Discord's are: Overview
// (name, topic) and Permissions: what this channel allows or denies @everyone and each role, on top of what they may do in the
// whole Pond (the Pond's docs/design.md, "Permissions"). Each permission is one of three: denied here, as everywhere (the
// slash), or allowed here. "Private" is the quick way to leave it to some roles: everyone is denied View, and the roles
// chosen allowed it. A Pond before protocol 13 only knows who sees a channel: everyone, or some roles.
ColumnLayout {
    id: ed
    required property var settings
    readonly property var ponds: settings.ponds
    property string channelId: ""
    signal back()
    spacing: 12

    readonly property var channel: find(channelId, ponds.channels.count)
    function find(id, count) {
        for (let i = 0; i < ponds.channels.count; i++) {
            const c = ponds.channels.get(i)
            if (c.id === id)
                return c
        }
        return null
    }
    property var overrides: []      // as being edited: [{role, allow: [], deny: []}], role "0" for everyone
    property string target: "0"     // whose overrides are shown
    property bool dirty: false
    function load() {
        const c = find(channelId) // not `channel`: its binding may not have caught up with channelId yet
        if (!c)
            return
        nameField.text = c.name
        topicField.text = c.topic
        overrides = JSON.parse(JSON.stringify(ponds.channelOverrides(channelId)))
        if (!overrides.some(o => o.role === "0"))
            overrides = [{ role: "0", allow: [], deny: [] }].concat(overrides)
        target = "0"
        dirty = false
    }
    onChannelIdChanged: load()
    Component.onCompleted: load()

    readonly property var permissions: [
        { id: "view_channels", title: qsTr("View channel"), note: qsTr("See it and read it.") },
        { id: "send_messages", title: qsTr("Send messages"), note: qsTr("Write in it.") },
        { id: "attach_files", title: qsTr("Attach files"), note: qsTr("Send pictures and files in it.") },
        { id: "manage_messages", title: qsTr("Manage messages"), note: qsTr("Delete anyone's messages in it."), edits: true },
        { id: "view_message_log", title: qsTr("View message log"), note: qsTr("See what was edited or deleted in it."), edits: true },
        { id: "connect", title: qsTr("Connect"), note: qsTr("Join it (a voice channel)."), voice: true },
        { id: "speak", title: qsTr("Speak"), note: qsTr("Be heard in it."), voice: true },
        { id: "video", title: qsTr("Video"), note: qsTr("Show a camera or share a screen in it."), voice: true },
        { id: "voice_mute", title: qsTr("Mute in voice"), note: qsTr("Mute someone below them here, for everyone."), voice: true },
        { id: "move_members", title: qsTr("Disconnect members"), note: qsTr("Take someone below them out of it."), voice: true }
    ]
    function entry(role) { return overrides.find(o => o.role === role) }
    function stateOf(role, perm) {
        const o = entry(role)
        return !o ? 0 : o.allow.includes(perm) ? 1 : o.deny.includes(perm) ? -1 : 0
    }
    function setState(role, perm, s) {
        const list = JSON.parse(JSON.stringify(overrides))
        let o = list.find(x => x.role === role)
        if (!o) { o = { role: role, allow: [], deny: [] }; list.push(o) }
        o.allow = o.allow.filter(p => p !== perm)
        o.deny = o.deny.filter(p => p !== perm)
        if (s > 0) o.allow.push(perm)
        if (s < 0) o.deny.push(perm)
        overrides = list
        dirty = true
    }
    function roleName(id) { return id === "0" ? "@everyone" : ponds.roleName(id) }
    function roleColor(id) {
        for (let i = 0; i < ponds.roles.count; i++)
            if (ponds.roles.get(i).id === id) return ponds.roles.get(i).color || ""
        return ""
    }
    // private: everyone can't see it; the roles that may, ticked
    readonly property bool isPrivate: stateOf("0", "view_channels") < 0
    function setPrivate(on) {
        setState("0", "view_channels", on ? -1 : 0)
    }
    function save() {
        if (nameField.text !== channel.name || topicField.text.trim() !== channel.topic)
            ponds.editChannel(channelId, nameField.text, topicField.text)
        if (ponds.canOverrides)
            ponds.setChannelOverrides(channelId, overrides)
        dirty = false
    }

    // ---- its header: back, and its name ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        ToolButton {
            objectName: "channelEditorBack"
            icon.source: Theme.icon("chevron-left"); icon.width: 18; icon.height: 18
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Back to the channels")
            ToolTip.text: Accessible.name; ToolTip.visible: hovered
            onClicked: ed.back()
        }
        Icon { glyph: ed.channel && ed.channel.kind === "voice" ? "volume" : "hash"; size: 18; opacity: 0.7 }
        Label { text: ed.channel ? ed.channel.name : ""; font.bold: true; font.pixelSize: 16; Layout.fillWidth: true }
        Button {
            text: qsTr("Delete channel")
            palette.buttonText: Theme.danger
            onClicked: ed.settings.confirmAsk(qsTr("Delete #%1?").arg(ed.channel.name), qsTr("Nobody will be able to read it any more. This can't be undone from the app."),
                                              qsTr("Delete channel"), (id => () => { ed.ponds.deleteChannel(id); ed.back() })(ed.channelId))
        }
    }
    TabStrip {
        id: tabs
        tabs: [{ text: qsTr("Overview"), name: "channelTabOverview" }, { text: qsTr("Permissions"), name: "channelTabPermissions", enabled: ed.ponds.canRoles }]
    }
    StackLayout {
        Layout.fillWidth: true
        currentIndex: tabs.currentIndex

        // overview
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("Name"); font.bold: true }
            TextField {
                id: nameField
                objectName: "channelNameField"
                Layout.preferredWidth: 300
                maximumLength: 32
                onTextEdited: { const t = text.toLowerCase().replace(/\s/g, "-").replace(/[^a-z0-9-]/g, ""); if (t !== text) text = t; ed.dirty = true }
            }
            Label { text: qsTr("Topic"); font.bold: true }
            TextArea {
                id: topicField
                Layout.fillWidth: true
                Layout.minimumHeight: 70
                wrapMode: TextEdit.Wrap
                onTextChanged: if (activeFocus) ed.dirty = true
                background: Rectangle { radius: Theme.radius; color: Theme.tint(Theme.text, Theme.hover); border.width: 1; border.color: Theme.hairline }
            }
        }

        // permissions
        RowLayout {
            Layout.fillWidth: true
            spacing: 18
            // a Pond before protocol 13: who sees it, and nothing else
            ColumnLayout {
                visible: !ed.ponds.canOverrides
                Layout.fillWidth: true
                spacing: 4
                Label { text: qsTr("This Pond only knows who sees a channel; update it for the rest."); opacity: 0.6; font.pixelSize: 12 }
                RadioButton { id: oldEveryone; text: qsTr("Everyone"); checked: ed.channel ? !ed.channel.restricted : true }
                RadioButton { id: oldSome; text: qsTr("Only these roles (and operators and administrators)"); checked: ed.channel ? ed.channel.restricted : false }
                Repeater {
                    model: oldSome.checked ? ed.ponds.roles : null
                    delegate: CheckBox {
                        required property var model
                        text: model.name
                        checked: ed.channel && (ed.channel.roles || []).indexOf(model.id) >= 0
                        onToggled: {
                            const r = (ed.channel.roles || []).filter(x => x !== model.id)
                            if (checked) r.push(model.id)
                            ed.ponds.setChannelRoles(ed.channelId, false, r)
                        }
                    }
                }
                Button { text: qsTr("Save who sees it"); onClicked: ed.ponds.setChannelRoles(ed.channelId, oldEveryone.checked, ed.channel.roles || []) }
            }

            // roles and @everyone, with what each has said here
            ColumnLayout {
                visible: ed.ponds.canOverrides
                Layout.preferredWidth: 200
                Layout.alignment: Qt.AlignTop
                spacing: 6
                Switch {
                    objectName: "channelPrivate"
                    text: qsTr("Private")
                    checked: ed.isPrivate
                    onToggled: ed.setPrivate(checked)
                    ToolTip.text: qsTr("Only the roles allowed to view it (below) see it, with operators and administrators.")
                    ToolTip.visible: hovered
                }
                Label { text: qsTr("ROLES"); font.pixelSize: 11; font.bold: true; opacity: 0.6; Layout.topMargin: 6 }
                Repeater {
                    model: ed.overrides
                    delegate: Rectangle {
                        required property var modelData
                        objectName: "overrideTarget_" + ed.roleName(modelData.role)
                        Layout.fillWidth: true
                        implicitHeight: 30
                        radius: Theme.radius
                        color: ed.target === modelData.role ? Theme.tint(Theme.text, Theme.selected) : tHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                        HoverHandler { id: tHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: ed.target = parent.modelData.role }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 6
                            Rectangle { width: 10; height: 10; radius: 5; color: ed.roleColor(parent.parent.modelData.role) || Theme.tint(Theme.text, 0.3) }
                            Label { text: ed.roleName(parent.parent.modelData.role); Layout.fillWidth: true; elide: Text.ElideRight }
                            ToolButton {
                                visible: parent.parent.modelData.role !== "0"
                                implicitWidth: 22; implicitHeight: 22; padding: 2
                                icon.source: Theme.icon("x"); icon.width: 12; icon.height: 12
                                display: AbstractButton.IconOnly
                                Accessible.name: qsTr("Forget what this channel says of %1").arg(ed.roleName(parent.parent.modelData.role))
                                onClicked: { ed.overrides = ed.overrides.filter(o => o.role !== parent.parent.modelData.role); ed.target = "0"; ed.dirty = true }
                            }
                        }
                    }
                }
                ComboBox {
                    id: addRole
                    objectName: "channelAddRole"
                    Layout.fillWidth: true
                    readonly property var choices: {
                        const out = []
                        for (let i = 0; i < ed.ponds.roles.count; i++) {
                            const r = ed.ponds.roles.get(i)
                            if (!ed.overrides.some(o => o.role === r.id))
                                out.push(r)
                        }
                        return out
                    }
                    model: [qsTr("Add a role…")].concat(choices.map(r => r.name))
                    onActivated: i => {
                        if (i > 0) {
                            const r = choices[i - 1]
                            ed.overrides = ed.overrides.concat([{ role: r.id, allow: ed.isPrivate ? ["view_channels"] : [], deny: [] }])
                            ed.target = r.id
                            ed.dirty = true
                        }
                        currentIndex = 0
                    }
                }
            }

            // what the chosen one may do here
            ColumnLayout {
                visible: ed.ponds.canOverrides
                Layout.fillWidth: true
                Layout.preferredWidth: 400
                Layout.alignment: Qt.AlignTop
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    opacity: 0.65
                    text: ed.target === "0" ? qsTr("For everyone in this channel. ✕ takes a permission away here, ✓ gives it here, / leaves it as the Pond's roles say.")
                                            : qsTr("For %1 in this channel: what it says wins over @everyone's here.").arg(ed.roleName(ed.target))
                }
                Repeater {
                    model: ed.permissions
                    delegate: RowLayout {
                        id: pRow
                        required property var modelData
                        visible: (!modelData.voice || (ed.channel && ed.channel.kind === "voice")) && (!modelData.edits || ed.ponds.canEditMessages)
                        Layout.fillWidth: true
                        Layout.preferredWidth: parent ? parent.width : 400 // the whole column, so the choices line up on the right
                        spacing: 10
                        readonly property int choice: ed.stateOf(ed.target, modelData.id)
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: pRow.modelData.title }
                            Label { text: pRow.modelData.note; font.pixelSize: 12; opacity: 0.6 }
                        }
                        // deny, as the roles say, allow: lined up on the right
                        Row {
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            spacing: 0
                            Repeater {
                                model: [{ s: -1, glyph: "x", tint: Theme.danger, name: qsTr("Deny") }, { s: 0, glyph: "", tint: Theme.text, name: qsTr("As the roles say") },
                                        { s: 1, glyph: "check", tint: Theme.primary, name: qsTr("Allow") }]
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property bool on: pRow.choice === modelData.s
                                    objectName: "override_" + pRow.modelData.id + "_" + (modelData.s < 0 ? "deny" : modelData.s > 0 ? "allow" : "inherit")
                                    width: 34; height: 28
                                    radius: 6
                                    color: on ? Theme.tint(modelData.tint, 0.85) : Theme.tint(Theme.text, Theme.hover)
                                    border.width: 1
                                    border.color: Theme.hairline
                                    Icon { anchors.centerIn: parent; visible: parent.modelData.glyph !== ""; glyph: parent.modelData.glyph || "x"; size: 14; color: parent.on ? "white" : palette.text }
                                    Label { anchors.centerIn: parent; visible: parent.modelData.glyph === ""; text: "/"; color: parent.on ? "white" : palette.text; font.bold: true }
                                    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: ed.setState(ed.target, pRow.modelData.id, parent.modelData.s) }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: pRow.modelData.title + ": " + modelData.name
                                    Accessible.checked: on
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Item { Layout.fillWidth: true }
        Label { visible: ed.dirty; text: qsTr("Unsaved changes"); opacity: 0.6; font.pixelSize: 12 }
        Button { text: qsTr("Reset"); enabled: ed.dirty; onClicked: ed.load() }
        PrimaryButton {
            objectName: "saveChannel"
            text: qsTr("Save")
            enabled: ed.dirty && nameField.text !== ""
            onClicked: ed.save()
        }
    }
}
