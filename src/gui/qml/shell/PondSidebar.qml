import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The left-hand list while a Pond is open: its name, which drops down the Pond's menu (Server Settings for an operator or
// administrator, how you appear there, your id, leaving), a role the Pond offers you if it does, then its pages (the Pond's
// docs/design.md, "Pages": its rules, a guide; a line under them) and its channels. Its banner, if it has one, is over its name. A voice
// channel (docs/ponds.md, "Voice channels") has a speaker, and who is in it underneath, lit while they talk; clicking it shows
// its messages, like any channel, and the phone beside it joins its voice (nothing to answer), or leaves it. While you are in
// one, the voice panel above your own row says which, with its controls (VoicePanel.qml); your row has the app's Settings.
Rectangle {
    id: bar

    objectName: "pondSidebar"
    color: "transparent"   // it is drawn on a card
    required property var chats
    property var backend: null       // who is talking (Backend.speaking)
    readonly property var ponds: chats.ponds
    readonly property var speaking: backend ? backend.speaking : []
    signal leaveRequested()
    signal settingsRequested()
    signal appSettingsRequested()      // the app's Settings (not the Pond's)
    signal voiceOpenRequested()      // the name in the voice panel was clicked
    property var video: null         // CallVideo (the voice panel)
    property var view: null          // the shell's video pane state (the voice panel)
    signal notice(string text)

    // Alt+Up and Alt+Down walk through the channels
    function step(delta) {
        const n = list.count
        if (n === 0) return
        let i = list.currentIndex < 0 ? (delta > 0 ? 0 : n - 1) : Math.max(0, Math.min(n - 1, list.currentIndex + delta))
        list.currentIndex = i
        list.currentItem.openIt()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // its banner (protocol 11): a picture over its name
        Item {
            objectName: "pondBannerBox"
            visible: bar.ponds.currentBanner !== ""
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? Math.round(width * 0.4) : 0
            // a plain band while the picture comes
            Rectangle { anchors.fill: parent; radius: Theme.cardRadius; color: Theme.tint(Theme.text, Theme.hover) }
            Avatar {
                id: pondBanner
                objectName: "pondBanner"
                animated: true
                anchors.fill: parent
                size: width
                chats: bar.chats
                hash: bar.ponds.currentBanner
                name: ""
                tint: "transparent"
                radius: Theme.cardRadius
                opacity: ready ? 1 : 0
            }
        }

        // the name: a click drops the Pond's menu down from it, as Discord's server menu does
        Rectangle {
            id: header
            objectName: "pondHeader"
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: nameHover.hovered || dropdown.opened ? Theme.tint(Theme.text, Theme.hover) : "transparent"
            HoverHandler { id: nameHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: dropdown.opened ? dropdown.close() : dropdown.open() }
            Accessible.role: Accessible.ButtonMenu
            Accessible.name: qsTr("%1 menu").arg(bar.ponds.currentName)
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                Label { objectName: "pondName"; text: bar.ponds.currentName; font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
                Icon { glyph: dropdown.opened ? "x" : "chevron-down"; size: 16; opacity: 0.7 }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }

            Popup {
                id: dropdown
                objectName: "pondDropdown"
                x: 8
                y: parent.height + 4
                width: parent.width - 16
                padding: 6
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                background: Rectangle {
                    radius: Theme.radiusLarge
                    color: Theme.card
                    border.width: 1
                    border.color: Theme.hairline
                }
                enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
                exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }

                contentItem: Column {
                    spacing: 2
                    PondMenuRow {
                        objectName: "serverSettings"
                        visible: bar.ponds.canAdmin
                        label: qsTr("Server Settings")
                        glyph: "settings"
                        onActivated: { dropdown.close(); bar.settingsRequested() }
                    }
                    PondMenuRow {
                        objectName: "appearAsItem"
                        label: qsTr("Appear as…")
                        glyph: "users"
                        onActivated: { dropdown.close(); appearMenu.popup(header, 8, header.height + 4) }
                    }
                    // direct messages (docs/pond-dms.md): whether members may write to us here, where the Pond passes them on
                    PondMenuRow {
                        objectName: "acceptDmsItem"
                        visible: bar.ponds.dmsOn
                        label: bar.ponds.acceptDms ? qsTr("Turn off direct messages") : qsTr("Allow direct messages")
                        glyph: "message"
                        onActivated: { dropdown.close(); bar.ponds.setAcceptDms(!bar.ponds.acceptDms) }
                    }
                    // our name in our role's colour, or our own: ours to choose, unless the Pond makes everyone's the role's
                    PondMenuRow {
                        objectName: "roleColorItem"
                        visible: bar.ponds.canRoleColors && !bar.ponds.roleColorsEnforced && bar.ponds.myRoleColor !== ""
                        label: bar.ponds.useRoleColor ? qsTr("Name in my own colour") : qsTr("Name in my role's colour")
                        glyph: "tag"
                        onActivated: { dropdown.close(); bar.ponds.setUseRoleColor(!bar.ponds.useRoleColor) }
                    }
                    PondMenuRow {
                        objectName: "copyMyId"
                        label: qsTr("Copy my id")
                        glyph: "link"
                        onActivated: { dropdown.close(); bar.chats.copy(bar.ponds.myId); bar.notice(qsTr("Your id is copied. The Pond's admin uses it to give you a role.")) }
                    }
                    Rectangle { width: parent.width; height: 9; color: "transparent"; Rectangle { anchors.centerIn: parent; width: parent.width - 12; height: 1; color: Theme.hairline } }
                    PondMenuRow {
                        objectName: "leavePond"
                        label: qsTr("Leave Pond")
                        glyph: "x"
                        danger: true
                        onActivated: { dropdown.close(); bar.leaveRequested() }
                    }
                }
            }
            Menu {
                id: appearMenu
                Repeater {
                    model: bar.chats.profiles
                    MenuItem { required property var model; text: model.display_name; onTriggered: bar.ponds.setProfile(bar.ponds.current, model.id) }
                }
                MenuSeparator {}
                MenuItem { text: qsTr("Nobody (only your id)"); onTriggered: bar.ponds.setProfile(bar.ponds.current, "") }
            }
        }

        // a role the Pond offers us: ours to take or leave
        Rectangle {
            objectName: "roleOffer"
            visible: bar.ponds.currentOffer !== ""
            Layout.fillWidth: true
            Layout.margins: 8
            implicitHeight: offerCol.implicitHeight + 20
            radius: Theme.radiusLarge
            color: Theme.tint(Theme.accent, 0.14)
            border.width: 1
            border.color: Theme.tint(Theme.accent, 0.5)
            ColumnLayout {
                id: offerCol
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.bold: true
                    text: bar.ponds.currentOffer === "operator" ? qsTr("%1 invites you to be an operator").arg(bar.ponds.currentName)
                                                               : qsTr("%1 invites you to be an administrator").arg(bar.ponds.currentName)
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.75
                    font.pixelSize: 12
                    text: bar.ponds.currentOffer === "operator" ? qsTr("Operators run the Pond: its channels, its members, and its administrators.")
                                                               : qsTr("Administrators look after the Pond: its channels and its members.")
                }
                RowLayout {
                    spacing: 8
                    PrimaryButton { objectName: "acceptOffer"; text: qsTr("Accept"); onClicked: bar.ponds.answerOffer(true) }
                    Button { objectName: "declineOffer"; text: qsTr("Decline"); onClicked: bar.ponds.answerOffer(false) }
                }
            }
        }

        // its pages, above the channels and set off from them by a line, as Discord's server guide is
        RowLayout {
            visible: bar.ponds.canPages && (bar.ponds.pages.count > 0 || bar.ponds.canManagePages)
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 10
            Layout.topMargin: 14
            Label { text: qsTr("PAGES"); font.pixelSize: 11; font.bold: true; opacity: 0.55; Layout.fillWidth: true }
            ToolButton {
                objectName: "newPage"
                visible: bar.ponds.canManagePages
                implicitWidth: 22; implicitHeight: 22; padding: 3
                icon.source: Theme.icon("plus"); icon.width: 14; icon.height: 14
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("New page")
                ToolTip.text: qsTr("New page"); ToolTip.visible: hovered; ToolTip.delay: 300
                onClicked: newPageDialog.ask()
            }
        }
        Repeater {
            model: bar.ponds.canPages ? bar.ponds.pages : null
            delegate: Rectangle {
                id: pageRow
                required property var model
                readonly property bool selected: bar.ponds.page === model.id
                objectName: "page_" + model.title
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                implicitHeight: 34
                radius: Theme.radius
                color: selected ? Theme.tint(Theme.text, Theme.selected) : pageHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                HoverHandler { id: pageHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: bar.ponds.openPage(pageRow.model.id) }
                Accessible.role: Accessible.Button
                Accessible.name: pageRow.model.title
                Accessible.onPressAction: bar.ponds.openPage(pageRow.model.id)
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 6
                    Icon {
                        glyph: ({ roles: "tag", events: "calendar", form: "clipboard-list", shop: "shopping-bag" })[pageRow.model.kind] || "file-text"
                        size: 18; opacity: pageRow.selected ? 1 : 0.6
                    }
                    Label { text: pageRow.model.title; Layout.fillWidth: true; elide: Text.ElideRight; opacity: pageRow.selected ? 1 : 0.8; font.bold: pageRow.selected }
                    Icon { visible: pageRow.model.restricted === true; glyph: "lock"; size: 13; opacity: 0.5 }
                }
            }
        }
        Rectangle {
            visible: bar.ponds.canPages && (bar.ponds.pages.count > 0 || bar.ponds.canManagePages)
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 8
            height: 1
            color: Theme.hairline
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.topMargin: 14
            Layout.bottomMargin: 4
            text: qsTr("CHANNELS")
            font.pixelSize: 11
            font.bold: true
            opacity: 0.55
        }

        ScrollList {
            id: list
            objectName: "channelList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            model: bar.ponds.channels
            currentIndex: -1
            delegate: Item {
                id: row
                required property var model
                required property int index
                readonly property bool selected: model.id === bar.ponds.channel && bar.ponds.page === ""
                readonly property bool voice: model.kind === "voice"
                readonly property var people: model.voice || []
                // we are in this one's voice (connecting, or connected)
                readonly property bool here: voice && bar.ponds.voicePond === bar.ponds.current && bar.ponds.voiceChannel === model.id
                                             && bar.ponds.voiceState !== "" && bar.ponds.voiceState !== "ended"
                // any channel: show its messages (a voice channel's too: joining its voice is the button beside it)
                function openIt() { bar.ponds.openChannel(model.id) }
                objectName: "channel_" + model.name
                width: ListView.view.width
                height: head.height + (people.length ? inVoice.implicitHeight + 4 : 0)
                Rectangle {
                    id: head
                    width: parent.width
                    height: 34
                    radius: Theme.radius
                    color: row.selected ? Theme.tint(Theme.text, Theme.selected) : rowHover.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent"
                    HoverHandler { id: rowHover }
                    TapHandler { onTapped: row.openIt() }
                    Accessible.role: Accessible.Button
                    Accessible.name: row.voice ? qsTr("Voice channel %1").arg(row.model.name) : "#" + row.model.name
                    Accessible.onPressAction: row.openIt()
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 6
                        Icon { glyph: row.voice ? "volume" : "hash"; size: 18; opacity: row.selected ? 1 : 0.6 }
                        Label { text: row.model.name; Layout.fillWidth: true; elide: Text.ElideRight; opacity: row.selected ? 1 : 0.8; font.bold: row.selected }
                        // only some roles see it (Discord's lock on a private channel)
                        Icon { visible: row.model.restricted === true; glyph: "lock"; size: 13; opacity: 0.5 }
                        // messages that mention us, not yet seen (protocol 15)
                        Rectangle {
                            objectName: "mentions_" + row.model.name
                            visible: (row.model.mentions || 0) > 0
                            implicitWidth: Math.max(18, mentionCount.implicitWidth + 10); implicitHeight: 18; width: implicitWidth; height: implicitHeight
                            radius: 9
                            color: Theme.danger
                            Label { id: mentionCount; anchors.centerIn: parent; text: (row.model.mentions || 0) > 99 ? "99+" : (row.model.mentions || 0); color: "white"; font.pixelSize: 11; font.bold: true }
                        }
                        // a voice channel: join its voice (nothing to answer), or leave it if we are in it
                        ToolButton {
                            objectName: "voiceJoin_" + row.model.name
                            visible: row.voice
                            implicitWidth: 26
                            implicitHeight: 26
                            padding: 4
                            icon.source: Theme.icon(row.here ? "phone-off" : "phone")
                            icon.width: 16
                            icon.height: 16
                            icon.color: row.here ? Theme.danger : Theme.primary
                            display: AbstractButton.IconOnly
                            onClicked: row.here ? bar.ponds.leaveVoice() : bar.ponds.joinVoice(row.model.id)
                            Accessible.name: row.here ? qsTr("Leave voice in %1").arg(row.model.name) : qsTr("Join voice in %1").arg(row.model.name)
                            ToolTip.text: row.here ? qsTr("Leave voice") : qsTr("Join voice")
                            ToolTip.visible: hovered
                            ToolTip.delay: 300
                        }
                    }
                    ToolTip.text: row.model.topic
                    ToolTip.visible: rowHover.hovered && row.model.topic !== ""
                    ToolTip.delay: 600
                }
                // who is in a voice channel: lit while they talk
                Column {
                    id: inVoice
                    objectName: "voiceIn_" + row.model.name
                    visible: row.people.length > 0
                    anchors.top: head.bottom
                    anchors.topMargin: 2
                    x: 30
                    width: parent.width - 34
                    spacing: 2
                    Repeater {
                        model: row.people
                        delegate: RowLayout {
                            id: person
                            required property var modelData
                            readonly property bool talking: bar.speaking.indexOf(modelData.isMe ? "self" : modelData.id) >= 0
                            objectName: "inVoice_" + modelData.name
                            width: inVoice.width
                            height: 26
                            spacing: 6
                            Rectangle {
                                implicitWidth: 24
                                implicitHeight: 24
                                radius: 12
                                color: "transparent"
                                border.width: 2
                                border.color: person.talking ? Theme.primary : "transparent"
                                Avatar { anchors.centerIn: parent; chats: bar.chats; name: person.modelData.name; hash: person.modelData.avatar || ""; tint: person.modelData.color || ""; size: 20 }
                            }
                            Label {
                                text: person.modelData.name
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                opacity: person.talking ? 1 : 0.75
                            }
                            Icon { visible: person.modelData.muted && !person.modelData.serverMuted; glyph: "microphone-off"; size: 14; opacity: 0.6 }
                            // muted by a moderator, or not let speak in this channel: in red, as Discord's server mute
                            Icon { objectName: "serverMuted_" + person.modelData.name; visible: person.modelData.serverMuted === true || person.modelData.speak === false; glyph: "microphone-off"; size: 14; color: Theme.danger }
                            Icon { visible: person.modelData.deafened; glyph: "headphones-off"; size: 14; opacity: 0.6 }
                        }
                    }
                }
            }
        }

        Label {
            visible: bar.ponds.channels.count === 0
            Layout.fillWidth: true
            Layout.margins: 16
            wrapMode: Text.Wrap
            opacity: 0.6
            text: bar.ponds.currentStatus === "connecting" ? qsTr("Connecting…") : qsTr("This Pond has no channels yet.")
        }

        // the voice you are in, sliding up above your row (VoicePanel.qml)
        VoicePanel {
            Layout.fillWidth: true
            chats: bar.chats
            video: bar.video
            view: bar.view
            onOpenRequested: bar.voiceOpenRequested()
        }

        // you: who you are here, and the settings (reachable whatever is going on, a voice channel too)
        Rectangle {
            objectName: "pondSelf"
            Layout.fillWidth: true
            implicitHeight: 56
            color: "transparent"
            Rectangle { width: parent.width; height: 1; color: Theme.hairline }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8
                // as this Pond sees us: the profile chosen for it (myName is read so a profile change redraws it)
                readonly property var me: (bar.chats.myName, bar.chats.meIn("pond:" + bar.ponds.current))
                Avatar { objectName: "pondSelfAvatar"; chats: bar.chats; name: parent.me.name; hash: parent.me.avatar; size: 32 }
                Label { text: parent.me.name !== "" ? parent.me.name : qsTr("You"); font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                ToolButton {
                    objectName: "pondSettingsButton"
                    icon.source: Theme.icon("settings"); icon.width: Theme.iconSize; icon.height: Theme.iconSize
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Settings")
                    ToolTip.text: qsTr("Settings"); ToolTip.visible: hovered
                    onClicked: bar.appSettingsRequested()
                }
            }
        }
    }

    // a new page: its title, then it opens to be written
    Dialog {
        id: newPageDialog
        property string kind: "markdown"
        function ask() { pageTitle.text = ""; kind = "markdown"; open(); pageTitle.forceActiveFocus() }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(400, parent ? parent.width - 24 : 400)
        title: qsTr("New page")
        ColumnLayout {
            width: newPageDialog.availableWidth
            spacing: 8
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Pages show above the channels: rules, a guide, links, and more.") }
            TextField {
                id: pageTitle
                objectName: "newPageTitle"
                Layout.fillWidth: true
                placeholderText: qsTr("Title, like Rules")
                maximumLength: 64
                onAccepted: if (text.trim() !== "") newPageDialog.accept()
            }
            // what kind of page (protocol 13): each adds to the markdown
            Repeater {
                model: bar.ponds.canPageKinds ? [
                    { id: "markdown", title: qsTr("Page"), note: qsTr("Markdown, pictures and files") },
                    { id: "roles", title: qsTr("Roles to pick"), note: qsTr("Members take roles for themselves") },
                    { id: "events", title: qsTr("Events"), note: qsTr("A calendar of what's planned") },
                    { id: "form", title: qsTr("Entry form"), note: qsTr("Questions those joining answer") },
                    { id: "shop", title: qsTr("Shop"), note: qsTr("Things to buy, or donations, through Stripe") }
                ] : []
                delegate: RadioButton {
                    required property var modelData
                    objectName: "newPageKind_" + modelData.id
                    text: modelData.title + " — " + modelData.note
                    checked: newPageDialog.kind === modelData.id
                    onClicked: newPageDialog.kind = modelData.id
                }
            }
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { objectName: "createPage"; text: qsTr("Create"); enabled: pageTitle.text.trim() !== ""; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
        onAccepted: if (pageTitle.text.trim() !== "") bar.ponds.createPage(pageTitle.text, kind)
    }
}
