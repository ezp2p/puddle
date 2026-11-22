import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"
import "../theme"

// Server Settings: what an operator or administrator runs from the app (the Pond's docs/design.md, "Roles"), or a member as far as
// their roles let them ("Permissions": each section is there for whoever may use it), over the window as
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
    // the icon: a picture is sent to our Puddle first (as for a profile), then named as the Pond's
    property bool iconBusy: false
    FileDialog {
        id: iconPicker
        title: qsTr("Choose an icon")
        nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.gif *.webp)")]
        onAccepted: { settings.iconBusy = true; settings.chats.uploadPicture(selectedFile, "pond-icon") }
    }
    FileDialog {
        id: bannerPicker
        title: qsTr("Choose a banner")
        nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.gif *.webp)")]
        onAccepted: { settings.iconBusy = true; settings.chats.uploadPicture(selectedFile, "pond-banner") }
    }
    Connections {
        target: settings.chats
        function onPictureUploaded(slot, hash, error) {
            if (slot !== "pond-icon" && slot !== "pond-banner")
                return
            settings.iconBusy = false
            if (error !== "")
                settings.say(error, true)
            else if (slot === "pond-banner")
                settings.ponds.setBanner(hash)
            else
                settings.ponds.setIcon(hash)
        }
    }
    Connections {
        target: settings.ponds
        function onAdminDone(what) { if (settings.opened) settings.say(what, false) }
        function onAdminFailed(why) { if (settings.opened) settings.say(why, true) }
        // the role went (taken away, or we left): nothing here is ours to change any more
        function onCurrentInfoChanged() {
            if (settings.opened && !settings.ponds.canAdmin)
                settings.close()
            else if (settings.opened && !settings.sections.some(s => s.id === settings.section))
                settings.section = settings.sections[0].id
        }
    }

    function show(which) {
        section = sections.some(s => s.id === which) ? which : sections.length ? sections[0].id : "overview"
        status = ""
        open()
    }
    onSectionChanged: { if (section === "bans") ponds.loadBans(); if (section === "invites") ponds.loadInvites() }
    onOpened: {
        overview.reset()
        madeLink = ""
        if (section === "bans") ponds.loadBans()
        if (section === "invites") ponds.loadInvites()
    }
    // the invite just made: its link, to copy and give out
    property string madeLink: ""
    Connections {
        target: settings.ponds
        function onInviteMade(link) { settings.madeLink = link; settings.chats.copy(link); settings.say(qsTr("Invite made, and its link copied."), false) }
    }
    // "in 3 days", "in 5 hours": when an invite stops working
    function fromNow(ms) {
        const s = Math.max(0, (ms - Date.now()) / 1000)
        const m = Math.max(1, Math.round(s / 60)), h = Math.round(s / 3600), d = Math.round(s / 86400)
        if (s < 3600) return m === 1 ? qsTr("in a minute") : qsTr("in %1 minutes").arg(m)
        if (s < 86400 * 2) return h === 1 ? qsTr("in an hour") : qsTr("in %1 hours").arg(h)
        return qsTr("in %1 days").arg(d)
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

    // Each section for whoever may use it; Roles only where the Pond has them (protocol 9: roles for channels, and the channels' order)
    readonly property bool giveRoles: ponds.canRoles && ponds.canManageRoles
    readonly property var sections: [
        { id: "overview", title: qsTr("Overview"), glyph: "settings", may: ponds.canManagePond },
        { id: "channels", title: qsTr("Channels"), glyph: "hash", may: ponds.canManageChannels },
        { id: "roles", title: qsTr("Roles"), glyph: "tag", may: giveRoles },
        { id: "members", title: qsTr("Members"), glyph: "users", may: giveRoles || ponds.canKick || ponds.canBan },
        { id: "invites", title: qsTr("Invites"), glyph: "user-plus", may: ponds.canManageInvites },
        { id: "bans", title: qsTr("Bans"), glyph: "x", may: ponds.canBan }
    ].filter(s => s.may)
    // what a role can let its members do (the Pond's docs/design.md, "Permissions"), in the Pond's order
    readonly property var permissionList: [
        { id: "manage_pond", title: qsTr("Manage the Pond"), note: qsTr("Change its name, description and icon.") },
        { id: "manage_channels", title: qsTr("Manage channels"), note: qsTr("Make, edit, delete and order channels, and choose who sees each.") },
        { id: "manage_roles", title: qsTr("Manage roles"), note: qsTr("Make, edit and order the roles below their own, and give them to members.") },
        { id: "manage_invites", title: qsTr("Manage invites"), note: qsTr("See, make and revoke invites.") },
        { id: "kick_members", title: qsTr("Kick members"), note: qsTr("Remove members below them, who can come back with an invite.") },
        { id: "ban_members", title: qsTr("Ban members"), note: qsTr("Ban members below them, see the bans, and unban.") },
        { id: "mute_members", title: qsTr("Mute members"), note: qsTr("Mute members below them, for a while or until unmuted.") },
        { id: "manage_pages", title: qsTr("Manage pages"), note: qsTr("Make, edit, order and delete the pages above the channels.") }
    ].filter(p => p.id !== "manage_pages" || ponds.canPages)
    function permissionTitles(ids) {
        const out = []
        for (const p of permissionList)
            if (ids.indexOf(p.id) >= 0)
                out.push(p.title)
        return out.length ? out.join(", ") : qsTr("Sees its channels; no other permissions")
    }
    // a role's colour, as Discord offers them; or none
    readonly property var roleColors: ["#1abc9c", "#2ecc71", "#3498db", "#9b59b6", "#e91e63", "#f1c40f", "#e67e22", "#e74c3c", "#95a5a6", "#607d8b"]
    // "Everyone", or the names of the roles that may see a channel
    function whoSees(restricted, roleIds) {
        if (!restricted)
            return qsTr("Everyone")
        const names = []
        for (let i = 0; i < roleIds.length; i++)
            names.push(ponds.roleName(roleIds[i]))
        return names.length ? qsTr("Admins and %1").arg(names.join(", ")) : qsTr("Operators and administrators only")
    }
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
                    text: settings.ponds.currentRole === "operator" ? qsTr("You are an operator here.")
                        : settings.ponds.currentRole === "administrator" ? qsTr("You are an administrator here.")
                        : qsTr("Your roles let you do what is here.")
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
                objectName: "settingsScroll" /* as Settings' own: the tests bring what they click into view */
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: pages.implicitHeight + 40
                StackLayout {
                    id: pages
                    x: 32
                    y: 8
                    width: scroller.width - 64
                    currentIndex: ["overview", "channels", "members", "bans", "invites", "roles"].indexOf(settings.section)

                    // ---- Overview ----
                    ColumnLayout {
                        id: overview
                        spacing: 16
                        function reset() { nameField.text = settings.ponds.currentName; descField.text = settings.ponds.currentDescription }
                        readonly property bool changed: nameField.text.trim() !== settings.ponds.currentName || descField.text.trim() !== settings.ponds.currentDescription
                        SettingsGroup {
                            title: qsTr("Icon")
                            Layout.fillWidth: true
                            visible: settings.ponds.canIcon
                            RowLayout {
                                spacing: 16
                                Rectangle {
                                    width: 64; height: 64; radius: 20
                                    color: Theme.tint(Theme.text, Theme.selected)
                                    Avatar {
                                        id: iconNow
                                        objectName: "pondSettingsIcon"
                                        anchors.fill: parent
                                        chats: settings.chats
                                        name: settings.ponds.currentName
                                        hash: settings.ponds.currentIcon
                                        radius: parent.radius
                                        visible: hash !== "" && ready
                                    }
                                    Label {
                                        visible: !iconNow.visible
                                        anchors.centerIn: parent
                                        text: String(settings.ponds.currentName).split(/\s+/).filter(w => w.length).slice(0, 3).map(w => w.charAt(0)).join("")
                                        font.bold: true; font.pixelSize: 20
                                    }
                                }
                                ColumnLayout {
                                    spacing: 6
                                    Label {
                                        text: qsTr("Shown on the Pond's button for every member. A PNG, JPEG, GIF or WebP of at most 1 MiB; square works best.")
                                        wrapMode: Text.Wrap; opacity: 0.7; font.pixelSize: 12
                                        Layout.fillWidth: true; Layout.maximumWidth: 360
                                    }
                                    RowLayout {
                                        spacing: 8
                                        PrimaryButton {
                                            objectName: "changePondIcon"
                                            text: settings.iconBusy ? qsTr("Sending…") : qsTr("Change icon")
                                            enabled: !settings.iconBusy
                                            onClicked: iconPicker.open()
                                        }
                                        Button {
                                            objectName: "removePondIcon"
                                            text: qsTr("Remove")
                                            enabled: settings.ponds.currentIcon !== "" && !settings.iconBusy
                                            onClicked: settings.ponds.setIcon("")
                                        }
                                    }
                                }
                            }
                        }
                        // the banner (protocol 11): over the Pond's name, above its pages and channels
                        SettingsGroup {
                            title: qsTr("Banner")
                            Layout.fillWidth: true
                            // a Pond too old for them says so, rather than the banner and pages being nowhere to be found
                            Label {
                                objectName: "pondTooOldForPages"
                                visible: !settings.ponds.canPages
                                text: qsTr("This Pond is too old for a banner or pages. Once it is updated, its banner is set here, and pages are made from the + over its channels.")
                                wrapMode: Text.Wrap; opacity: 0.7; font.pixelSize: 12
                                Layout.fillWidth: true; Layout.maximumWidth: 520
                            }
                            RowLayout {
                                visible: settings.ponds.canPages
                                spacing: 16
                                Rectangle {
                                    width: 160; height: 64; radius: Theme.radius
                                    color: Theme.tint(Theme.text, Theme.selected)
                                    Avatar {
                                        id: bannerNow
                                        objectName: "pondSettingsBanner"
                                        animated: true
                                        anchors.fill: parent
                                        chats: settings.chats
                                        name: ""
                                        hash: settings.ponds.currentBanner
                                        tint: "transparent"
                                        radius: parent.radius
                                        visible: hash !== "" && ready
                                    }
                                    Icon { visible: !bannerNow.visible; anchors.centerIn: parent; glyph: "photo"; size: 22; opacity: 0.5 }
                                }
                                ColumnLayout {
                                    spacing: 6
                                    Label {
                                        text: qsTr("Shown over the Pond's name, above its pages and channels. A wide picture works best (about 5 by 2).")
                                        wrapMode: Text.Wrap; opacity: 0.7; font.pixelSize: 12
                                        Layout.fillWidth: true; Layout.maximumWidth: 360
                                    }
                                    RowLayout {
                                        spacing: 8
                                        PrimaryButton {
                                            objectName: "changePondBanner"
                                            text: settings.iconBusy ? qsTr("Sending…") : qsTr("Change banner")
                                            enabled: !settings.iconBusy
                                            onClicked: bannerPicker.open()
                                        }
                                        Button {
                                            objectName: "removePondBanner"
                                            text: qsTr("Remove")
                                            enabled: settings.ponds.currentBanner !== "" && !settings.iconBusy
                                            onClicked: settings.ponds.setBanner("")
                                        }
                                    }
                                }
                            }
                        }
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
                            // text, or voice (a channel to drop into and talk: docs/ponds.md, "Voice channels")
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 16
                                RadioButton {
                                    id: kindText
                                    objectName: "newChannelText"
                                    text: qsTr("Text")
                                    checked: true
                                    icon.source: Theme.icon("hash")
                                }
                                RadioButton {
                                    id: kindVoice
                                    objectName: "newChannelVoice"
                                    text: qsTr("Voice")
                                    enabled: settings.ponds.canVoice
                                    icon.source: Theme.icon("volume")
                                    ToolTip.text: qsTr("This Pond doesn't have voice channels.")
                                    ToolTip.visible: !enabled && voiceHover.hovered
                                    HoverHandler { id: voiceHover }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    opacity: 0.6
                                    font.pixelSize: 12
                                    text: kindVoice.checked ? qsTr("Members drop in and talk. It takes messages too.") : qsTr("Messages and files.")
                                }
                            }
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
                                    onClicked: {
                                        settings.ponds.createChannel(newName.text, newTopic.text, kindVoice.checked ? "voice" : "text")
                                        newName.text = ""
                                        newTopic.text = ""
                                        kindText.checked = true
                                    }
                                }
                            }
                        }
                        SettingsGroup {
                            title: qsTr("Channels")
                            note: settings.ponds.canRoles ? qsTr("Everyone sees the channels in this order. A channel can be left to some roles: operators and administrators always see every channel.") : ""
                            Layout.fillWidth: true
                            Repeater {
                                id: channelList
                                model: settings.ponds.channels
                                delegate: ColumnLayout {
                                    id: chRow
                                    required property var model
                                    required property int index
                                    property bool editing: false
                                    // who may see it, as being edited: everyone, or these roles
                                    property bool editEveryone: true
                                    property var editRoles: []
                                    objectName: "channelRow_" + model.name
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        visible: !chRow.editing
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Icon { glyph: chRow.model.kind === "voice" ? "volume" : "hash"; size: 16; opacity: 0.6 }
                                        Label { text: chRow.model.name; font.bold: true }
                                        Icon { visible: chRow.model.restricted; glyph: "lock"; size: 14; opacity: 0.6 }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            Label { visible: text !== ""; text: chRow.model.topic; opacity: 0.6; elide: Text.ElideRight; Layout.fillWidth: true }
                                            Label {
                                                visible: settings.ponds.canRoles
                                                text: settings.whoSees(chRow.model.restricted, chRow.model.roles || [])
                                                font.pixelSize: 11; opacity: 0.55; elide: Text.ElideRight; Layout.fillWidth: true
                                            }
                                        }
                                        ToolButton {
                                            objectName: "moveChannelUp_" + chRow.model.name
                                            visible: settings.ponds.canRoles
                                            enabled: chRow.index > 0
                                            icon.source: Theme.icon("arrow-up"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Move #%1 up").arg(chRow.model.name)
                                            ToolTip.text: qsTr("Move up"); ToolTip.visible: hovered
                                            onClicked: settings.ponds.moveChannel(chRow.model.id, -1)
                                        }
                                        ToolButton {
                                            objectName: "moveChannelDown_" + chRow.model.name
                                            visible: settings.ponds.canRoles
                                            enabled: chRow.index < channelList.count - 1
                                            icon.source: Theme.icon("arrow-down"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Move #%1 down").arg(chRow.model.name)
                                            ToolTip.text: qsTr("Move down"); ToolTip.visible: hovered
                                            onClicked: settings.ponds.moveChannel(chRow.model.id, 1)
                                        }
                                        ToolButton {
                                            icon.source: Theme.icon("pencil"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Edit #%1").arg(chRow.model.name)
                                            ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered
                                            onClicked: {
                                                editName.text = chRow.model.name
                                                editTopic.text = chRow.model.topic
                                                chRow.editEveryone = !chRow.model.restricted
                                                chRow.editRoles = (chRow.model.roles || []).slice()
                                                chRow.editing = true
                                            }
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
                                            onClicked: {
                                                if (editName.text !== chRow.model.name || editTopic.text.trim() !== chRow.model.topic)
                                                    settings.ponds.editChannel(chRow.model.id, editName.text, editTopic.text)
                                                const was = (chRow.model.roles || []).slice().sort().join(",")
                                                if (settings.ponds.canRoles && (chRow.editEveryone === chRow.model.restricted
                                                                                || (!chRow.editEveryone && chRow.editRoles.slice().sort().join(",") !== was)))
                                                    settings.ponds.setChannelRoles(chRow.model.id, chRow.editEveryone, chRow.editRoles)
                                                chRow.editing = false
                                            }
                                        }
                                        Button { text: qsTr("Cancel"); onClicked: chRow.editing = false }
                                    }
                                    // who may see it
                                    ColumnLayout {
                                        visible: chRow.editing && settings.ponds.canRoles
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 4
                                        spacing: 2
                                        Label { text: qsTr("Who can see it"); font.bold: true; font.pixelSize: 12 }
                                        RadioButton {
                                            objectName: "channelEveryone_" + chRow.model.name
                                            text: qsTr("Everyone")
                                            checked: chRow.editEveryone
                                            onToggled: chRow.editEveryone = true
                                        }
                                        RadioButton {
                                            objectName: "channelSomeRoles_" + chRow.model.name
                                            text: qsTr("Only these roles (and operators and administrators)")
                                            checked: !chRow.editEveryone
                                            onToggled: chRow.editEveryone = false
                                        }
                                        Flow {
                                            visible: !chRow.editEveryone
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 28
                                            spacing: 4
                                            Label {
                                                visible: settings.ponds.roles.count === 0
                                                text: qsTr("There are no roles yet: make them under Roles. Until then only operators and administrators see it.")
                                                wrapMode: Text.Wrap; opacity: 0.6; font.pixelSize: 12
                                                width: parent.width
                                            }
                                            Repeater {
                                                model: settings.ponds.roles
                                                delegate: CheckBox {
                                                    required property var model
                                                    text: model.name
                                                    checked: chRow.editRoles.indexOf(model.id) >= 0
                                                    onToggled: {
                                                        const r = chRow.editRoles.filter(x => x !== model.id)
                                                        if (checked)
                                                            r.push(model.id)
                                                        chRow.editRoles = r
                                                    }
                                                }
                                            }
                                        }
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
                            note: qsTr("Removing someone lets them back with a new invite; banning them does not, until they are unbanned. Operators are given and taken away by the server's admin (pondctl). Someone whose roles let them remove or ban can only do it to members whose highest role is below theirs.")
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
                                        Avatar { chats: settings.chats; name: mRow.model.name; hash: mRow.model.avatar || ""; tint: mRow.model.color || ""; size: 32 }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            RowLayout {
                                                spacing: 6
                                                Label { text: mRow.model.name + (mRow.model.isMe ? qsTr(" (you)") : ""); font.bold: true }
                                                RoleBadge { role: mRow.model.role }
                                            }
                                            Label { text: mRow.model.id; font.pixelSize: 11; opacity: 0.5; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                            // their roles for channels
                                            Flow {
                                                visible: (mRow.model.roles || []).length > 0
                                                Layout.fillWidth: true
                                                Layout.topMargin: 2
                                                spacing: 4
                                                Repeater {
                                                    model: { mRow.model.roles; return settings.ponds.rolesOf(mRow.model.id) }
                                                    delegate: Rectangle {
                                                        required property var modelData
                                                        height: 20; radius: 10
                                                        width: chip.implicitWidth + 16
                                                        color: Theme.tint(Theme.text, Theme.selected)
                                                        Row {
                                                            id: chip
                                                            anchors.centerIn: parent
                                                            spacing: 4
                                                            Rectangle {
                                                                visible: parent.parent.modelData.color !== ""
                                                                anchors.verticalCenter: parent.verticalCenter
                                                                width: 8; height: 8; radius: 4
                                                                color: parent.parent.modelData.color || "transparent"
                                                            }
                                                            Label { text: parent.parent.modelData.name; font.pixelSize: 11 }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        Button {
                                            objectName: "memberRoles_" + mRow.model.name
                                            visible: settings.giveRoles
                                            text: qsTr("Roles")
                                            icon.source: Theme.icon("chevron-down"); icon.width: 14; icon.height: 14
                                            enabled: settings.ponds.roles.count > 0
                                            ToolTip.text: qsTr("Make roles under Roles first."); ToolTip.visible: !enabled && rolesHover.hovered
                                            HoverHandler { id: rolesHover }
                                            onClicked: rolesMenu.popup()
                                            Menu {
                                                id: rolesMenu
                                                Repeater {
                                                    model: settings.ponds.roles
                                                    delegate: MenuItem {
                                                        required property var model
                                                        text: model.name
                                                        checkable: true
                                                        enabled: model.editable === true
                                                        checked: (mRow.model.roles || []).indexOf(model.id) >= 0
                                                        onTriggered: settings.ponds.setMemberRole(mRow.model.id, model.id, checked)
                                                    }
                                                }
                                            }
                                        }
                                        Button {
                                            text: qsTr("Remove")
                                            visible: settings.ponds.canKick
                                            enabled: mRow.canAct
                                            onClicked: confirm.ask(qsTr("Remove %1?").arg(mRow.model.name),
                                                                   qsTr("They leave the Pond now, and can come back with a new invite."),
                                                                   qsTr("Remove"), () => settings.ponds.kick(mRow.model.id))
                                        }
                                        Button {
                                            text: qsTr("Ban")
                                            visible: settings.ponds.canBan
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

                    // ---- Invites ----
                    ColumnLayout {
                        spacing: 16
                        SettingsGroup {
                            title: qsTr("Make an invite")
                            note: qsTr("Anyone with the link can join, until it is used up, expires or is revoked.")
                            Layout.fillWidth: true
                            RowLayout {
                                spacing: 16
                                ColumnLayout {
                                    spacing: 4
                                    Label { text: qsTr("Expire after"); font.bold: true; font.pixelSize: 12 }
                                    ComboBox {
                                        id: expiry
                                        objectName: "inviteExpiry"
                                        textRole: "text"
                                        valueRole: "value"
                                        currentIndex: 5
                                        model: [
                                            { text: qsTr("30 minutes"), value: 1800 }, { text: qsTr("1 hour"), value: 3600 },
                                            { text: qsTr("6 hours"), value: 21600 }, { text: qsTr("12 hours"), value: 43200 },
                                            { text: qsTr("1 day"), value: 86400 }, { text: qsTr("7 days"), value: 604800 },
                                            { text: qsTr("Never"), value: 0 }
                                        ]
                                    }
                                }
                                ColumnLayout {
                                    spacing: 4
                                    Label { text: qsTr("Number of uses"); font.bold: true; font.pixelSize: 12 }
                                    ComboBox {
                                        id: uses
                                        objectName: "inviteUses"
                                        textRole: "text"
                                        valueRole: "value"
                                        model: [
                                            { text: qsTr("No limit"), value: 0 }, { text: qsTr("1 use"), value: 1 }, { text: qsTr("5 uses"), value: 5 },
                                            { text: qsTr("10 uses"), value: 10 }, { text: qsTr("25 uses"), value: 25 }, { text: qsTr("100 uses"), value: 100 }
                                        ]
                                    }
                                }
                                ColumnLayout {
                                    spacing: 4
                                    Label { text: " "; font.pixelSize: 12 }
                                    PrimaryButton {
                                        objectName: "createInvite"
                                        text: qsTr("Make invite")
                                        onClicked: settings.ponds.createInvite(uses.currentValue, expiry.currentValue)
                                    }
                                }
                            }
                            // the one just made: copied already, and here to copy again
                            RowLayout {
                                visible: settings.madeLink !== ""
                                Layout.fillWidth: true
                                Layout.topMargin: 6
                                spacing: 8
                                TextField {
                                    objectName: "madeInviteLink"
                                    Layout.fillWidth: true
                                    readOnly: true
                                    text: settings.madeLink
                                    selectByMouse: true
                                    onActiveFocusChanged: if (activeFocus) selectAll()
                                }
                                Button { objectName: "copyMadeInvite"; text: qsTr("Copy"); onClicked: { settings.chats.copy(settings.madeLink); settings.say(qsTr("Link copied."), false) } }
                            }
                        }
                        SettingsGroup {
                            title: qsTr("Invites (%1)").arg(settings.ponds.invites.count)
                            note: qsTr("Those that can still be used, made here or with pondctl. Revoking one stops it working at once; nobody who joined with it leaves.")
                            Layout.fillWidth: true
                            Label { visible: settings.ponds.invites.count === 0; text: qsTr("No invites can be used right now."); opacity: 0.6 }
                            Repeater {
                                model: settings.ponds.invites
                                delegate: ColumnLayout {
                                    id: iRow
                                    required property var model
                                    objectName: "inviteRow"
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        ColumnLayout {
                                            spacing: 2
                                            Layout.fillWidth: true
                                            Label { text: iRow.model.token; font.family: "monospace"; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                            Label {
                                                Layout.fillWidth: true
                                                wrapMode: Text.Wrap
                                                font.pixelSize: 12
                                                opacity: 0.65
                                                text: (iRow.model.maxUses > 0 ? qsTr("Used %1 of %2").arg(iRow.model.uses).arg(iRow.model.maxUses)
                                                                              : iRow.model.uses === 1 ? qsTr("Used once, no limit")
                                                                              : qsTr("Used %1 times, no limit").arg(iRow.model.uses))
                                                      + " · " + (iRow.model.expires > 0 ? qsTr("expires %1").arg(settings.fromNow(iRow.model.expires)) : qsTr("never expires"))
                                            }
                                        }
                                        Button { objectName: "copyInvite"; text: qsTr("Copy link"); onClicked: { settings.chats.copy(iRow.model.link); settings.say(qsTr("Link copied."), false) } }
                                        Button {
                                            objectName: "revokeInvite"
                                            text: qsTr("Revoke")
                                            onClicked: confirm.ask(qsTr("Revoke this invite?"),
                                                                   qsTr("The link stops working at once. Anyone who joined with it stays."),
                                                                   qsTr("Revoke"), () => settings.ponds.revokeInvite(iRow.model.token))
                                        }
                                    }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
                                }
                            }
                        }
                    }

                    // ---- Roles (the Pond's docs/design.md, "Roles for channels" and "Permissions") ----
                    ColumnLayout {
                        spacing: 16
                        SettingsGroup {
                            title: qsTr("Make a role")
                            note: settings.ponds.canPermissions
                                  ? qsTr("Roles say who can see a channel, and what their members may do. Give them to members under Members, and choose a channel's roles under Channels. Higher roles come first: someone whose roles let them manage roles, kick, ban or mute can only do it below their own highest role. Operators and administrators may do everything and see every channel.")
                                  : qsTr("Roles say who can see a channel: give them to members under Members, and choose a channel's roles under Channels. They give no say over the Pond; operators and administrators see every channel.")
                            Layout.fillWidth: true
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                TextField {
                                    id: newRole
                                    objectName: "newRoleName"
                                    Layout.preferredWidth: 260
                                    placeholderText: qsTr("name, like Moderators")
                                    maximumLength: 32
                                    onAccepted: if (text.trim() !== "") { settings.ponds.createRole(text); text = "" }
                                }
                                PrimaryButton {
                                    objectName: "createRole"
                                    text: qsTr("Create")
                                    enabled: newRole.text.trim() !== ""
                                    onClicked: { settings.ponds.createRole(newRole.text); newRole.text = "" }
                                }
                            }
                        }
                        // names in a role's colour (protocol 12): each member's choice, unless the Pond makes it everyone's
                        SettingsGroup {
                            title: qsTr("Name colours")
                            note: qsTr("Members choose whether their name shows in their highest coloured role's colour or in their own, from the Pond's menu. You can make everyone's show in their role's colour instead.")
                            Layout.fillWidth: true
                            visible: settings.ponds.canRoleColors && settings.ponds.canManagePond
                            CheckBox {
                                objectName: "enforceRoleColors"
                                text: qsTr("Everyone's name in their role's colour")
                                checked: settings.ponds.roleColorsEnforced
                                onToggled: {
                                    settings.ponds.setRoleColorsEnforced(checked)
                                    checked = Qt.binding(() => settings.ponds.roleColorsEnforced) // until the Pond says so
                                }
                            }
                        }
                        SettingsGroup {
                            title: qsTr("Roles (%1)").arg(settings.ponds.roles.count)
                            Layout.fillWidth: true
                            Label { visible: settings.ponds.roles.count === 0; text: qsTr("No roles yet."); opacity: 0.6 }
                            Repeater {
                                id: roleRows
                                model: settings.ponds.roles
                                delegate: ColumnLayout {
                                    id: rRow
                                    required property var model
                                    required property int index
                                    property bool editing: false
                                    // what the editor holds until Save
                                    property string editColor: ""
                                    property bool editHoist: false
                                    property var editPermissions: []
                                    readonly property bool editable: model.editable === true
                                    function startEdit() {
                                        roleName.text = model.name
                                        editColor = model.color || ""
                                        editHoist = model.hoist === true
                                        editPermissions = (model.permissions || []).slice()
                                        colorField.text = editColor
                                        editing = true
                                    }
                                    function togglePermission(id, on) {
                                        const p = editPermissions.filter(x => x !== id)
                                        if (on)
                                            p.push(id)
                                        editPermissions = p
                                    }
                                    objectName: "roleRow_" + model.name
                                    Layout.fillWidth: true
                                    spacing: 6
                                    RowLayout {
                                        visible: !rRow.editing
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Rectangle {
                                            width: 14; height: 14; radius: 7
                                            color: rRow.model.color || "transparent"
                                            border.width: rRow.model.color ? 0 : 1
                                            border.color: Theme.hairline
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            RowLayout {
                                                spacing: 8
                                                Label { text: rRow.model.name; font.bold: true; color: rRow.model.color || palette.text }
                                                Label {
                                                    opacity: 0.6
                                                    text: rRow.model.count === 1 ? qsTr("1 member") : qsTr("%1 members").arg(rRow.model.count)
                                                }
                                                Label {
                                                    visible: rRow.model.hoist === true
                                                    text: qsTr("shown apart")
                                                    font.pixelSize: 11
                                                    opacity: 0.6
                                                }
                                            }
                                            Label {
                                                objectName: "rolePermissions_" + rRow.model.name
                                                visible: settings.ponds.canPermissions
                                                Layout.fillWidth: true
                                                text: settings.permissionTitles(rRow.model.permissions || [])
                                                font.pixelSize: 12
                                                opacity: 0.6
                                                elide: Text.ElideRight
                                            }
                                        }
                                        // its place in the order, higher roles first; below our highest, unless we have a rank
                                        ToolButton {
                                            objectName: "moveRoleUp_" + rRow.model.name
                                            visible: settings.ponds.canPermissions
                                            enabled: rRow.editable && rRow.index > 0 && settings.ponds.roleEditableAt(rRow.index - 1)
                                            icon.source: Theme.icon("arrow-up"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Move %1 up").arg(rRow.model.name)
                                            ToolTip.text: qsTr("Move up"); ToolTip.visible: hovered
                                            onClicked: settings.ponds.moveRole(rRow.model.id, -1)
                                        }
                                        ToolButton {
                                            objectName: "moveRoleDown_" + rRow.model.name
                                            visible: settings.ponds.canPermissions
                                            enabled: rRow.editable && rRow.index < settings.ponds.roles.count - 1
                                            icon.source: Theme.icon("arrow-down"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Move %1 down").arg(rRow.model.name)
                                            ToolTip.text: qsTr("Move down"); ToolTip.visible: hovered
                                            onClicked: settings.ponds.moveRole(rRow.model.id, 1)
                                        }
                                        ToolButton {
                                            objectName: "editRole_" + rRow.model.name
                                            enabled: rRow.editable
                                            icon.source: Theme.icon("pencil"); icon.width: 16; icon.height: 16
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Edit %1").arg(rRow.model.name)
                                            ToolTip.text: rRow.editable ? qsTr("Edit") : qsTr("Only roles below your highest"); ToolTip.visible: hovered
                                            onClicked: rRow.startEdit()
                                        }
                                        ToolButton {
                                            enabled: rRow.editable
                                            icon.source: Theme.icon("trash"); icon.width: 16; icon.height: 16; icon.color: Theme.danger
                                            display: AbstractButton.IconOnly
                                            Accessible.name: qsTr("Delete %1").arg(rRow.model.name)
                                            ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered
                                            onClicked: confirm.ask(qsTr("Delete %1?").arg(rRow.model.name),
                                                                   qsTr("Members lose it. A channel only this role could see is then left to operators and administrators."),
                                                                   qsTr("Delete role"), () => settings.ponds.deleteRole(rRow.model.id))
                                        }
                                    }
                                    // the editor: its name, and where the Pond has them, its colour, whether it is shown apart, and permissions
                                    ColumnLayout {
                                        visible: rRow.editing
                                        Layout.fillWidth: true
                                        spacing: 10
                                        RowLayout {
                                            spacing: 8
                                            Label { text: qsTr("Name"); Layout.preferredWidth: 90 }
                                            TextField { id: roleName; objectName: "roleNameField"; Layout.preferredWidth: 260; maximumLength: 32 }
                                        }
                                        RowLayout {
                                            visible: settings.ponds.canPermissions
                                            spacing: 6
                                            Label { text: qsTr("Colour"); Layout.preferredWidth: 90 }
                                            // none
                                            Rectangle {
                                                width: 24; height: 24; radius: 12
                                                color: "transparent"
                                                border.width: rRow.editColor === "" ? 2 : 1
                                                border.color: rRow.editColor === "" ? Theme.primary : Theme.hairline
                                                Icon { anchors.centerIn: parent; glyph: "x"; size: 12; opacity: 0.6 }
                                                TapHandler { onTapped: { rRow.editColor = ""; colorField.text = "" } }
                                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                                Accessible.role: Accessible.Button
                                                Accessible.name: qsTr("No colour")
                                            }
                                            Repeater {
                                                model: settings.roleColors
                                                delegate: Rectangle {
                                                    required property string modelData
                                                    objectName: "roleColor_" + modelData
                                                    width: 24; height: 24; radius: 12
                                                    color: modelData
                                                    border.width: rRow.editColor === modelData ? 2 : 0
                                                    border.color: palette.text
                                                    TapHandler { onTapped: { rRow.editColor = parent.modelData; colorField.text = parent.modelData } }
                                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                                    Accessible.role: Accessible.Button
                                                    Accessible.name: modelData
                                                }
                                            }
                                            TextField {
                                                id: colorField
                                                Layout.preferredWidth: 90
                                                placeholderText: "#rrggbb"
                                                maximumLength: 7
                                                validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{0,6})?$/ }
                                                onTextEdited: if (/^#[0-9a-fA-F]{6}$/.test(text) || text === "") rRow.editColor = text.toLowerCase()
                                            }
                                        }
                                        RowLayout {
                                            visible: settings.ponds.canPermissions
                                            spacing: 8
                                            Item { Layout.preferredWidth: 90 }
                                            CheckBox {
                                                objectName: "roleHoist"
                                                text: qsTr("Show members with this role apart in the member list")
                                                checked: rRow.editHoist
                                                onToggled: rRow.editHoist = checked
                                            }
                                        }
                                        ColumnLayout {
                                            visible: settings.ponds.canPermissions
                                            spacing: 2
                                            Label { text: qsTr("Permissions"); font.bold: true; Layout.topMargin: 4 }
                                            Repeater {
                                                model: settings.permissionList
                                                delegate: RowLayout {
                                                    required property var modelData
                                                    spacing: 8
                                                    Layout.fillWidth: true
                                                    // only what we may do ourselves can be added
                                                    readonly property bool mine: settings.ponds.hasPermission(modelData.id)
                                                    CheckBox {
                                                        objectName: "rolePermission_" + parent.modelData.id
                                                        checked: rRow.editPermissions.indexOf(parent.modelData.id) >= 0
                                                        enabled: parent.mine || checked
                                                        onToggled: rRow.togglePermission(parent.modelData.id, checked)
                                                    }
                                                    ColumnLayout {
                                                        spacing: 0
                                                        Layout.fillWidth: true
                                                        Label { text: parent.parent.modelData.title }
                                                        Label {
                                                            Layout.fillWidth: true
                                                            text: parent.parent.modelData.note
                                                            font.pixelSize: 12
                                                            opacity: 0.6
                                                            wrapMode: Text.Wrap
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        RowLayout {
                                            spacing: 8
                                            PrimaryButton {
                                                objectName: "saveRole"
                                                text: qsTr("Save")
                                                enabled: roleName.text.trim() !== ""
                                                onClicked: {
                                                    if (settings.ponds.canPermissions)
                                                        settings.ponds.editRole(rRow.model.id, { name: roleName.text, color: rRow.editColor, hoist: rRow.editHoist,
                                                                                                 permissions: rRow.editPermissions })
                                                    else
                                                        settings.ponds.renameRole(rRow.model.id, roleName.text)
                                                    rRow.editing = false
                                                }
                                            }
                                            Button { text: qsTr("Cancel"); onClicked: rRow.editing = false }
                                        }
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
