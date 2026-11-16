import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The left-hand list while a Pond is open: its name, which drops down the Pond's menu (Server Settings for an operator or
// administrator, how you appear there, your id, leaving), a role the Pond offers you if it does, then its channels. A voice
// channel (docs/ponds.md, "Voice channels") has a speaker, and who is in it underneath, lit while they talk; clicking it shows
// its messages, like any channel, and the phone beside it joins its voice (nothing to answer), or leaves it. While you are in
// one, in any Pond, a bar at the bottom says which, with mute, deafen and leave.
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
                readonly property bool selected: model.id === bar.ponds.channel
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
                                Avatar { anchors.centerIn: parent; chats: bar.chats; name: person.modelData.name; tint: person.modelData.color || ""; size: 20 }
                            }
                            Label {
                                text: person.modelData.name
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                opacity: person.talking ? 1 : 0.75
                            }
                            Icon { visible: person.modelData.muted; glyph: "microphone-off"; size: 14; opacity: 0.6 }
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

        // the voice channel we are in, whichever Pond it is in
        Rectangle {
            objectName: "voiceBar"
            visible: bar.ponds.voiceState !== ""
            Layout.fillWidth: true
            implicitHeight: voiceRow.implicitHeight + 16
            color: Theme.tint(Theme.text, 0.04)
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.hairline }
            RowLayout {
                id: voiceRow
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 4
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        objectName: "voiceStatus"
                        font.bold: true
                        font.pixelSize: 13
                        color: bar.ponds.voiceState === "active" ? Theme.primary : bar.ponds.voiceState === "ended" ? Theme.danger : palette.windowText
                        text: bar.ponds.voiceState === "active" ? qsTr("Voice connected")
                              : bar.ponds.voiceState === "connecting" ? qsTr("Connecting…")
                              : bar.ponds.voiceReason === "removed" ? qsTr("Disconnected by the Pond") : qsTr("Voice disconnected")
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        opacity: 0.7
                        text: bar.ponds.voiceName + (bar.ponds.voicePondName !== "" ? " / " + bar.ponds.voicePondName : "")
                    }
                }
                ToolButton {
                    objectName: "voiceMute"
                    visible: bar.ponds.voiceState !== "ended"
                    icon.source: Theme.icon(bar.ponds.voiceMuted ? "microphone-off" : "microphone")
                    highlighted: bar.ponds.voiceMuted
                    onClicked: bar.ponds.setVoiceMuted(!bar.ponds.voiceMuted)
                    Accessible.name: bar.ponds.voiceMuted ? qsTr("Unmute") : qsTr("Mute")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                }
                ToolButton {
                    objectName: "voiceDeafen"
                    visible: bar.ponds.voiceState !== "ended"
                    icon.source: Theme.icon(bar.ponds.voiceDeafened ? "headphones-off" : "headphones")
                    highlighted: bar.ponds.voiceDeafened
                    onClicked: bar.ponds.setVoiceDeafened(!bar.ponds.voiceDeafened)
                    Accessible.name: bar.ponds.voiceDeafened ? qsTr("Undeafen") : qsTr("Deafen")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                }
                ToolButton {
                    objectName: "voiceLeave"
                    icon.source: Theme.icon(bar.ponds.voiceState === "ended" ? "x" : "phone-off")
                    onClicked: bar.ponds.leaveVoice()
                    Accessible.name: bar.ponds.voiceState === "ended" ? qsTr("Dismiss") : qsTr("Leave the voice channel")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                }
            }
        }
    }
}
