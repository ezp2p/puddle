import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Server Settings, Roles (the Pond's docs/design.md, "Permissions"), laid out as Discord's: the roles on the left, highest first,
// with @everyone under them; the one chosen on the right, in tabs: Display (name, colour, shown apart), Permissions (grouped,
// each a switch) and Members (who holds it). Someone without a rank changes only roles below their own highest, and adds only
// permissions they have.
RowLayout {
    id: pane
    required property var settings
    readonly property var ponds: settings.ponds
    spacing: 20

    property string chosen: ""         // a role's id, or "everyone"
    property int chosenIndex: -1
    readonly property var role: chosenIndex >= 0 && chosenIndex < ponds.roles.count ? ponds.roles.get(chosenIndex) : null
    // what the editor holds until Save
    property string editName: ""
    property string editColor: ""
    property bool editHoist: false
    property var editPermissions: []
    property bool dirty: false

    function choose(id, index) {
        chosen = id
        chosenIndex = index
        if (id === "everyone") {
            editPermissions = ponds.everyone.slice()
            tabs.currentIndex = 1
        } else {
            const r = ponds.roles.get(index)
            editName = r.name
            editColor = r.color || ""
            editHoist = r.hoist === true
            editPermissions = (r.permissions || []).slice()
            nameField.text = editName
            colorField.text = editColor
        }
        dirty = false
    }
    function toggle(id, on) {
        const p = editPermissions.filter(x => x !== id)
        if (on)
            p.push(id)
        editPermissions = p
        dirty = true
    }
    function save() {
        if (chosen === "everyone")
            ponds.setEveryone(editPermissions)
        else if (ponds.canPermissions)
            ponds.editRole(chosen, { name: nameField.text, color: editColor, hoist: editHoist, permissions: editPermissions })
        else
            ponds.renameRole(chosen, nameField.text)
        dirty = false
    }
    // the role chosen moved or went: follow it by id
    Connections {
        target: pane.ponds.roles
        function onCountChanged() { pane.relocate() }
        function onModelReset() { pane.relocate() }
    }
    function relocate() {
        if (chosen === "" || chosen === "everyone")
            return
        for (let i = 0; i < ponds.roles.count; i++)
            if (ponds.roles.get(i).id === chosen) {
                chosenIndex = i
                return
            }
        chosen = ""
        chosenIndex = -1
    }

    // the permissions, in groups, as Discord shows them; those of protocol 13 only where the Pond has them
    readonly property var groups: [
        { title: qsTr("General"), items: [
            { id: "view_channels", title: qsTr("View channels"), note: qsTr("See the channels (unless a channel says otherwise) and read them."), p13: true },
            { id: "manage_channels", title: qsTr("Manage channels"), note: qsTr("Make, edit, delete and order channels, and set their permissions.") },
            { id: "manage_roles", title: qsTr("Manage roles"), note: qsTr("Make, edit and order the roles below their own, and give them to members.") },
            { id: "manage_pond", title: qsTr("Manage the Pond"), note: qsTr("Change its name, description, icon, banner, and connect the shop to Stripe.") },
            { id: "manage_pages", title: qsTr("Manage pages"), note: qsTr("Make, edit, order and delete the pages above the channels."), pages: true },
            { id: "manage_invites", title: qsTr("Manage invites"), note: qsTr("See, make and revoke invites.") }
        ] },
        { title: qsTr("Membership"), items: [
            { id: "kick_members", title: qsTr("Kick members"), note: qsTr("Remove members below them, who can come back with an invite.") },
            { id: "ban_members", title: qsTr("Ban members"), note: qsTr("Ban members below them, see the bans, and unban.") },
            { id: "mute_members", title: qsTr("Mute members"), note: qsTr("Stop members below them writing, for a while or until unmuted.") },
            { id: "manage_applications", title: qsTr("Manage applications"), note: qsTr("Read the answers of those applying to join, and accept or decline them."), p13: true }
        ] },
        { title: qsTr("Text"), items: [
            { id: "send_messages", title: qsTr("Send messages"), note: qsTr("Write in the channels they see."), p13: true },
            { id: "attach_files", title: qsTr("Attach files"), note: qsTr("Send pictures and files with their messages."), p13: true }
        ] },
        { title: qsTr("Voice"), items: [
            { id: "connect", title: qsTr("Connect"), note: qsTr("Join voice channels."), p13: true },
            { id: "speak", title: qsTr("Speak"), note: qsTr("Be heard in voice channels."), p13: true },
            { id: "video", title: qsTr("Video"), note: qsTr("Show their camera and share their screen."), p13: true },
            { id: "voice_mute", title: qsTr("Mute in voice"), note: qsTr("Mute someone below them in a voice channel, for everyone."), p13: true },
            { id: "move_members", title: qsTr("Disconnect members"), note: qsTr("Take someone below them out of a voice channel."), p13: true }
        ] },
        { title: qsTr("Events and shop"), items: [
            { id: "manage_events", title: qsTr("Manage events"), note: qsTr("Add, change and delete events on events pages."), p13: true },
            { id: "manage_shop", title: qsTr("Manage the shop"), note: qsTr("Add, change and take off items, and see every order."), p13: true }
        ] }
    ]
    function shown(item) { return (!item.p13 || ponds.canOverrides) && (!item.pages || ponds.canPages) }

    // ---- the roles ----
    ColumnLayout {
        Layout.preferredWidth: 240
        Layout.fillHeight: true
        Layout.alignment: Qt.AlignTop
        spacing: 8
        RowLayout {
            spacing: 6
            TextField {
                id: newRole
                objectName: "newRoleName"
                Layout.fillWidth: true
                placeholderText: qsTr("New role, like Moderators")
                maximumLength: 32
                onAccepted: if (text.trim() !== "") { pane.ponds.createRole(text); text = "" }
            }
            ToolButton {
                objectName: "createRole"
                enabled: newRole.text.trim() !== ""
                icon.source: Theme.icon("plus"); icon.width: 16; icon.height: 16
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Create role")
                ToolTip.text: Accessible.name; ToolTip.visible: hovered
                onClicked: { pane.ponds.createRole(newRole.text); newRole.text = "" }
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 11
            opacity: 0.6
            text: pane.ponds.canPermissions ? qsTr("Higher roles first. Members get everything @everyone may, and what each of their roles adds.")
                                            : qsTr("Roles say who sees a channel.")
        }
        Repeater {
            id: roleRows
            model: pane.ponds.roles
            delegate: Rectangle {
                id: rRow
                required property var model
                required property int index
                readonly property bool selected: pane.chosen === model.id
                readonly property bool editable: model.editable === true
                objectName: "editRole_" + model.name
                Layout.fillWidth: true
                implicitHeight: 34
                radius: Theme.radius
                color: selected ? Theme.tint(Theme.text, Theme.selected) : rowHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: pane.choose(rRow.model.id, rRow.index) }
                Accessible.role: Accessible.Button
                Accessible.name: model.name
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 2
                    spacing: 6
                    Rectangle { width: 12; height: 12; radius: 6; color: rRow.model.color || Theme.tint(Theme.text, 0.3) }
                    Label { text: rRow.model.name; Layout.fillWidth: true; elide: Text.ElideRight; color: rRow.model.color || palette.text; font.bold: rRow.selected }
                    Label { text: rRow.model.count; opacity: 0.5; font.pixelSize: 11 }
                    ToolButton {
                        objectName: "moveRoleUp_" + rRow.model.name
                        visible: pane.ponds.canPermissions
                        opacity: rRow.selected || rowHover.hovered ? 1 : 0.35
                        // (read again whenever the roles change: count and the row's own editable go with them)
                        enabled: rRow.editable && rRow.index > 0 && pane.ponds.roles.count > 1 && pane.ponds.roleEditableAt(rRow.index - 1)
                        implicitWidth: 24; implicitHeight: 24; padding: 2
                        icon.source: Theme.icon("arrow-up"); icon.width: 14; icon.height: 14
                        display: AbstractButton.IconOnly
                        Accessible.name: qsTr("Move %1 up").arg(rRow.model.name)
                        onClicked: pane.ponds.moveRole(rRow.model.id, -1)
                    }
                    ToolButton {
                        objectName: "moveRoleDown_" + rRow.model.name
                        visible: pane.ponds.canPermissions
                        opacity: rRow.selected || rowHover.hovered ? 1 : 0.35
                        enabled: rRow.editable && rRow.index < pane.ponds.roles.count - 1
                        implicitWidth: 24; implicitHeight: 24; padding: 2
                        icon.source: Theme.icon("arrow-down"); icon.width: 14; icon.height: 14
                        display: AbstractButton.IconOnly
                        Accessible.name: qsTr("Move %1 down").arg(rRow.model.name)
                        onClicked: pane.ponds.moveRole(rRow.model.id, 1)
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline; visible: pane.ponds.canOverrides }
        Rectangle {
            objectName: "editRole_everyone"
            visible: pane.ponds.canOverrides
            Layout.fillWidth: true
            implicitHeight: 34
            radius: Theme.radius
            color: pane.chosen === "everyone" ? Theme.tint(Theme.text, Theme.selected) : everyoneHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
            HoverHandler { id: everyoneHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: pane.choose("everyone", -1) }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                spacing: 6
                Icon { glyph: "users"; size: 14; opacity: 0.7 }
                Label { text: "@everyone"; Layout.fillWidth: true; font.bold: pane.chosen === "everyone" }
            }
        }
        // names in a role's colour (protocol 12): each member's choice, unless the Pond makes it everyone's
        CheckBox {
            objectName: "enforceRoleColors"
            visible: pane.ponds.canRoleColors && pane.ponds.canManagePond
            Layout.topMargin: 8
            text: qsTr("Everyone's name in their role's colour")
            checked: pane.ponds.roleColorsEnforced
            onToggled: {
                pane.ponds.setRoleColorsEnforced(checked)
                checked = Qt.binding(() => pane.ponds.roleColorsEnforced) // until the Pond says so
            }
            ToolTip.text: qsTr("Otherwise each member chooses, from the Pond's menu.")
            ToolTip.visible: hovered
        }
        Item { Layout.fillHeight: true }
    }

    // ---- the one chosen ----
    ColumnLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        spacing: 12
        Label {
            visible: pane.chosen === ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.6
            text: pane.ponds.roles.count === 0 ? qsTr("No roles yet: name one on the left.") : qsTr("Choose a role on the left to change it.")
        }
        RowLayout {
            visible: pane.chosen !== ""
            Layout.fillWidth: true
            Label {
                text: pane.chosen === "everyone" ? "@everyone" : (pane.role ? pane.role.name : "")
                font.bold: true; font.pixelSize: 16
                color: pane.chosen !== "everyone" && pane.role && pane.role.color ? pane.role.color : palette.text
                Layout.fillWidth: true
            }
            Label {
                visible: pane.chosen !== "everyone" && pane.role && pane.role.editable !== true
                text: qsTr("Above your highest role: you can't change it")
                opacity: 0.6; font.pixelSize: 12
            }
            ToolButton {
                visible: pane.chosen !== "everyone" && pane.role && pane.role.editable === true
                icon.source: Theme.icon("trash"); icon.width: 16; icon.height: 16; icon.color: Theme.danger
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Delete role")
                ToolTip.text: Accessible.name; ToolTip.visible: hovered
                onClicked: pane.settings.confirmAsk(qsTr("Delete %1?").arg(pane.role.name),
                                                    qsTr("Members lose it, and channels forget what they said of it."),
                                                    qsTr("Delete role"), (id => () => pane.ponds.deleteRole(id))(pane.chosen))
            }
        }
        TabStrip {
            id: tabs
            visible: pane.chosen !== ""
            tabs: [{ text: qsTr("Display"), name: "roleTabDisplay", enabled: pane.chosen !== "everyone" },
                   { text: qsTr("Permissions"), name: "roleTabPermissions", enabled: pane.ponds.canPermissions },
                   { text: qsTr("Members"), name: "roleTabMembers", enabled: pane.chosen !== "everyone" }]
        }
        StackLayout {
            visible: pane.chosen !== ""
            Layout.fillWidth: true
            currentIndex: tabs.currentIndex
            enabled: pane.chosen === "everyone" || (pane.role && pane.role.editable === true)

            // display
            ColumnLayout {
                spacing: 12
                Label { text: qsTr("Name"); font.bold: true }
                TextField { id: nameField; objectName: "roleNameField"; Layout.preferredWidth: 300; maximumLength: 32; onTextEdited: pane.dirty = true }
                Label { visible: pane.ponds.canPermissions; text: qsTr("Colour"); font.bold: true }
                Flow {
                    visible: pane.ponds.canPermissions
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle {
                        width: 28; height: 28; radius: 14
                        color: "transparent"
                        border.width: pane.editColor === "" ? 2 : 1
                        border.color: pane.editColor === "" ? Theme.primary : Theme.hairline
                        Icon { anchors.centerIn: parent; glyph: "x"; size: 12; opacity: 0.6 }
                        TapHandler { onTapped: { pane.editColor = ""; colorField.text = ""; pane.dirty = true } }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("No colour")
                    }
                    Repeater {
                        model: pane.settings.roleColors
                        delegate: Rectangle {
                            required property string modelData
                            objectName: "roleColor_" + modelData
                            width: 28; height: 28; radius: 14
                            color: modelData
                            border.width: pane.editColor === modelData ? 3 : 0
                            border.color: palette.text
                            TapHandler { onTapped: { pane.editColor = parent.modelData; colorField.text = parent.modelData; pane.dirty = true } }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData
                        }
                    }
                    TextField {
                        id: colorField
                        width: 100
                        placeholderText: "#rrggbb"
                        maximumLength: 7
                        validator: RegularExpressionValidator { regularExpression: /^(#[0-9a-fA-F]{0,6})?$/ }
                        onTextEdited: if (/^#[0-9a-fA-F]{6}$/.test(text) || text === "") { pane.editColor = text.toLowerCase(); pane.dirty = true }
                    }
                }
                Switch {
                    objectName: "roleHoist"
                    visible: pane.ponds.canPermissions
                    text: qsTr("Show members with this role apart in the member list")
                    checked: pane.editHoist
                    onToggled: { pane.editHoist = checked; pane.dirty = true }
                }
            }

            // permissions
            ColumnLayout {
                spacing: 4
                Label {
                    visible: pane.chosen === "everyone"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: qsTr("What every member may do, before their roles add more. A channel can still take any of it away, or give it, in its own permissions.")
                }
                Repeater {
                    model: pane.groups
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        visible: modelData.items.some(i => pane.shown(i))
                        Label { text: modelData.title.toUpperCase(); font.pixelSize: 11; font.bold: true; opacity: 0.6; Layout.topMargin: 10 }
                        Repeater {
                            model: parent.modelData.items
                            delegate: RowLayout {
                                id: pRow
                                required property var modelData
                                visible: pane.shown(modelData)
                                Layout.fillWidth: true
                                spacing: 10
                                // only what we may do ourselves can be added
                                // (read again whenever what we hold there changes: currentRole goes with it)
                                readonly property bool mine: pane.ponds.currentRole !== "" && pane.ponds.everyone !== undefined && pane.ponds.hasPermission(modelData.id)
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: pRow.modelData.title }
                                    Label { Layout.fillWidth: true; text: pRow.modelData.note; font.pixelSize: 12; opacity: 0.6; wrapMode: Text.Wrap }
                                }
                                Switch {
                                    objectName: "rolePermission_" + pRow.modelData.id
                                    checked: pane.editPermissions.indexOf(pRow.modelData.id) >= 0
                                    enabled: pRow.mine || checked
                                    onToggled: pane.toggle(pRow.modelData.id, checked)
                                    Accessible.name: pRow.modelData.title
                                }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline; Layout.topMargin: 6 }
                    }
                }
            }

            // members
            ColumnLayout {
                spacing: 4
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; opacity: 0.65; font.pixelSize: 12; text: qsTr("Tick those who hold it.") }
                Repeater {
                    model: pane.chosen !== "everyone" ? pane.ponds.members : null
                    delegate: CheckBox {
                        required property var model
                        objectName: "roleMember_" + model.name
                        text: model.name
                        checked: (model.roles || []).indexOf(pane.chosen) >= 0
                        enabled: pane.ponds.canManageRoles
                        onToggled: pane.ponds.setMemberRole(model.id, pane.chosen, checked)
                    }
                }
            }
        }
        RowLayout {
            visible: pane.chosen !== "" && tabs.currentIndex < 2
            Item { Layout.fillWidth: true }
            Label { visible: pane.dirty; text: qsTr("Unsaved changes"); opacity: 0.6; font.pixelSize: 12 }
            Button { text: qsTr("Reset"); enabled: pane.dirty; onClicked: pane.choose(pane.chosen, pane.chosenIndex) }
            PrimaryButton {
                objectName: "saveRole"
                text: qsTr("Save")
                enabled: pane.chosen === "everyone" || nameField.text.trim() !== ""
                onClicked: pane.save()
            }
        }
    }
}
