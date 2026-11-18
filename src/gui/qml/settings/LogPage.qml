import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"
import "../dialogs"
import "../theme"

// Settings, and what the Puddle has been doing, newest at the bottom.
Pane {
    id: page

    required property var backend
    required property var chats
    property bool narrow: false
    property bool sidebarShown: true
    signal toggleSidebar()

    padding: 0
    background: null

    ConfirmDialog { id: confirm }
    HandOverDialog { id: handOver; backend: page.backend }
    property string backupNote: ""
    FileDialog {
        id: backupSaver
        title: qsTr("Save a backup of your Puddle")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "puddle"
        nameFilters: [qsTr("Puddle backup (*.puddle)")]
        onAccepted: { page.backupNote = ""; page.backend.makeBackup(selectedFile) }
    }
    Connections {
        target: page.backend
        function onBackupFinished(error, file, handover) {
            if (!handover)
                page.backupNote = error !== "" ? qsTr("The backup failed: %1").arg(error)
                                               : qsTr("Saved to %1. Only your recovery phrase opens it.").arg(file)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            PanelToggle { glyph: "layout-sidebar"; label: qsTr("Conversations"); active: page.sidebarShown; onClicked: page.toggleSidebar() }
            Label { text: qsTr("Settings"); font.bold: true; font.pixelSize: 16 }
            Item { Layout.fillWidth: true }
            Label {
                text: page.backend.phone ? qsTr("Connected to %1").arg(page.backend.phoneLink.address)
                      : page.backend.error === "" ? qsTr("Listening on %1").arg(page.backend.listenUrl) : qsTr("Not running")
                opacity: 0.6
                elide: Text.ElideMiddle
                Layout.maximumWidth: page.width * 0.6
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
        ScrollArea {
            id: settingsScroll
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: sections.implicitHeight
            ColumnLayout {
                id: sections
                width: settingsScroll.width
                spacing: 0

                // Like Discord's Developer Mode: what is normally kept out of sight
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: qsTr("Developer Mode"); font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("Show user IDs and codes, and let you copy them.")
                        }
                    }
                    Switch {
                        objectName: "developerSwitch"
                        checked: page.backend.developerMode
                        onToggled: page.backend.developerMode = checked
                        Accessible.name: qsTr("Developer Mode")
                    }
                }
                RowLayout {
                    visible: page.backend.developerMode && page.backend.myId !== ""
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 8
                    Label { text: qsTr("Your user ID"); opacity: 0.65 }
                    TextField {
                        objectName: "myIdField"
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: page.backend.myId
                        font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
                        font.pixelSize: 12
                        Accessible.name: qsTr("Your user ID")
                        FieldMenu {}
                    }
                    Button { text: qsTr("Copy"); onClicked: page.backend.copyText(page.backend.myId) }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }

                // how the microphone goes in a Pond's voice channel (docs/ponds.md, "Voice channels"); a call always sends it
                ColumnLayout {
                    objectName: "voiceSettings"
                    visible: !page.backend.phone
                    Layout.fillWidth: true
                    Layout.margins: 16
                    spacing: 6
                    Label { text: qsTr("Voice channels"); font.bold: true }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("When your microphone is heard in a Pond's voice channel. Calls always send it, unless you mute.")
                    }
                    RowLayout {
                        spacing: 16
                        RadioButton {
                            objectName: "talkVoice"
                            text: qsTr("Voice activation")
                            checked: page.backend.talkMode === "voice"
                            onToggled: if (checked) page.backend.talkMode = "voice"
                        }
                        RadioButton {
                            objectName: "talkPush"
                            text: qsTr("Push to talk")
                            checked: page.backend.talkMode === "push"
                            onToggled: if (checked) page.backend.talkMode = "push"
                        }
                    }
                    // voice activation: how loud you must be; the bar under it is how loud you are now, while in a voice channel
                    ColumnLayout {
                        visible: page.backend.talkMode === "voice"
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("Sensitivity"); opacity: 0.8 }
                            Slider {
                                id: threshold
                                objectName: "talkThreshold"
                                Layout.fillWidth: true
                                from: -80
                                to: 0
                                value: page.backend.talkThreshold
                                onMoved: page.backend.talkThreshold = value
                                Accessible.name: qsTr("Voice activation sensitivity")
                            }
                            Label { text: qsTr("%1 dB").arg(Math.round(threshold.value)); opacity: 0.6; font.pixelSize: 12 }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 80
                            implicitHeight: 4
                            radius: 2
                            color: Theme.tint(Theme.text, 0.1)
                            Rectangle {
                                height: parent.height
                                radius: 2
                                width: parent.width * Math.max(0, Math.min(1, (page.backend.micLevel + 80) / 80))
                                color: page.backend.micLevel >= page.backend.talkThreshold ? Theme.primary : Theme.tint(Theme.text, 0.35)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 80
                            wrapMode: Text.Wrap
                            opacity: 0.55
                            font.pixelSize: 11
                            text: qsTr("Join a voice channel to see how loud you are. Green is heard.")
                        }
                    }
                    // the video pane: everyone kept in place (the default, as a streamer wants), or those talking first
                    RowLayout {
                        spacing: 16
                        Label { text: qsTr("Video"); opacity: 0.8 }
                        RadioButton {
                            objectName: "videoArrival"
                            text: qsTr("Keep everyone in place")
                            checked: page.backend.videoOrder === "arrival"
                            onToggled: if (checked) page.backend.videoOrder = "arrival"
                        }
                        RadioButton {
                            objectName: "videoSpeaking"
                            text: qsTr("Who's talking first")
                            checked: page.backend.videoOrder === "speaking"
                            onToggled: if (checked) page.backend.videoOrder = "speaking"
                        }
                    }
                    // push to talk: the key, chosen by pressing it
                    RowLayout {
                        visible: page.backend.talkMode === "push"
                        spacing: 8
                        Label { text: qsTr("Key"); opacity: 0.8 }
                        Button {
                            id: keyButton
                            objectName: "pushKeyButton"
                            property bool waiting: false
                            text: waiting ? qsTr("Press a key…") : page.backend.pushKeyName !== "" ? page.backend.pushKeyName : qsTr("Choose a key")
                            onClicked: { waiting = true; forceActiveFocus() }
                            onActiveFocusChanged: if (!activeFocus) waiting = false
                            Keys.onPressed: event => {
                                if (!waiting)
                                    return
                                if (event.key !== Qt.Key_Escape)
                                    page.backend.pushKey = event.key
                                waiting = false
                                event.accepted = true
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.55
                            font.pixelSize: 11
                            text: qsTr("Hold it while the window is in front to be heard.")
                        }
                    }
                }
                Rectangle { visible: !page.backend.phone; Layout.fillWidth: true; height: 1; color: Theme.hairline }

                // phones: on the computer, let them in; on a phone, let go of the Puddle (docs/identity.md, "Devices")
                ColumnLayout {
                    objectName: "phones"
                    visible: !page.backend.phone
                    Layout.fillWidth: true
                    Layout.margins: 16
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Phones"); font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button {
                            objectName: "addPhoneButton"
                            text: qsTr("Add a phone")
                            visible: page.chats.pairingLink === ""
                            onClicked: page.chats.startPairing()
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: qsTr("A phone you add uses this Puddle: it can do everything this window can, except add or remove phones. "
                                   + "Your keys and your chats stay on this computer.")
                    }
                    ColumnLayout {
                        visible: page.chats.pairingLink !== ""
                        Layout.fillWidth: true
                        spacing: 8
                        Image {
                            visible: page.backend.canDrawQr
                            Layout.alignment: Qt.AlignHCenter
                            sourceSize: Qt.size(220, 220)
                            source: page.chats.pairingLink === "" ? "" : "image://qr/" + encodeURIComponent(page.chats.pairingLink)
                            smooth: false
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: page.backend.canDrawQr ? qsTr("On the phone, open Puddle and scan this, or copy the link to it. It works once, for ten minutes.")
                                                         : qsTr("On the phone, open Puddle and paste this link. It works once, for ten minutes.")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                objectName: "pairingLinkField"
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                text: page.chats.pairingLink
                                font.pixelSize: 12
                                FieldMenu {}
                            }
                            Button { text: qsTr("Copy"); onClicked: page.backend.copyText(page.chats.pairingLink) }
                            Button { text: qsTr("Cancel"); onClicked: page.chats.stopPairing() }
                        }
                    }
                    Repeater {
                        model: page.chats.devices
                        delegate: RowLayout {
                            required property var model
                            Layout.fillWidth: true
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label { text: model.name; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 12
                                    color: model.state === "pending" ? Theme.warning : palette.text
                                    opacity: model.state === "pending" ? 1 : 0.65
                                    text: model.state === "pending"
                                          ? qsTr("Wants to use your Puddle. Accept it only if the phone shows %1.").arg(model.check.slice(0, 3) + " " + model.check.slice(3))
                                          : model.last_seen ? qsTr("Last used %1").arg(new Date(model.last_seen).toLocaleString(Qt.locale(), Locale.ShortFormat))
                                          : qsTr("Added %1").arg(new Date(model.created_at).toLocaleString(Qt.locale(), Locale.ShortFormat))
                                }
                            }
                            Button {
                                objectName: "acceptPhone"
                                visible: model.state === "pending"
                                text: qsTr("Accept")
                                highlighted: true
                                onClicked: page.chats.acceptDevice(model.id)
                            }
                            Button {
                                visible: model.state === "pending"
                                text: qsTr("Decline")
                                onClicked: page.chats.declineDevice(model.id)
                            }
                            Button {
                                visible: model.state !== "pending"
                                text: qsTr("Remove")
                                onClicked: page.chats.revokeDevice(model.id)
                            }
                        }
                    }
                }
                RowLayout {
                    visible: page.backend.phone
                    Layout.fillWidth: true
                    Layout.margins: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: page.backend.modeFixed ? qsTr("This phone") : qsTr("This device"); font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: (page.backend.modeFixed
                                   ? qsTr("Uses your Puddle at %1. Disconnecting forgets it here; remove the phone on your Puddle too.")
                                   : qsTr("Uses your Puddle at %1. Disconnecting forgets it here (remove this device on your Puddle too); "
                                          + "then you can pair again, or run a Puddle on this computer instead."))
                                  .arg(page.backend.phone ? page.backend.phoneLink.address : "")
                        }
                    }
                    Button {
                        text: qsTr("Disconnect")
                        onClicked: confirm.ask(qsTr("Disconnect from your Puddle?"),
                                               qsTr("This device forgets your Puddle and its key. To use it again, pair it again with a new link."),
                                               qsTr("Disconnect"), () => page.backend.phoneLink.forget())
                    }
                }
                // the Puddle runs here: the owner may want this app to use one somewhere else instead
                RowLayout {
                    objectName: "thisComputer"
                    visible: !page.backend.phone && !page.backend.modeFixed
                    Layout.fillWidth: true
                    Layout.margins: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: qsTr("This computer"); font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.65
                            font.pixelSize: 12
                            text: qsTr("Your Puddle runs here. A backup holds all of it (keys, friends, chats, files, phones and "
                                       + "profiles) in one file that only your recovery phrase opens. To have it run on a server instead, "
                                       + "move it there: this app then uses it.")
                        }
                        Label {
                            visible: page.backend.backingUp || page.backupNote !== ""
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            text: page.backend.backingUp ? qsTr("Writing the backup…") : page.backupNote
                        }
                        RowLayout {
                            spacing: 8
                            Button {
                                objectName: "backupButton"
                                text: qsTr("Back up…")
                                enabled: page.backend.running && !page.backend.backingUp
                                onClicked: backupSaver.open()
                            }
                            Button {
                                objectName: "moveButton"
                                text: qsTr("Move to a server…")
                                enabled: page.backend.running && !page.backend.backingUp
                                onClicked: handOver.show()
                            }
                        }
                    }
                    Button {
                        Layout.alignment: Qt.AlignTop
                        text: qsTr("Change…")
                        onClicked: confirm.ask(qsTr("Stop running your Puddle here?"),
                                               qsTr("Puddle restarts and asks where your Puddle is. The one on this computer stops, so friends "
                                                    + "can't reach it here, but nothing is deleted: its identity, friends and chats stay in %1, "
                                                    + "and come back if you choose this computer again.").arg(page.backend.dataDir),
                                               qsTr("Restart and choose"), () => page.backend.askAgain())
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.hairline }
                // people you blocked, and a way to let them back
                Label { visible: page.chats.blocked.count > 0; Layout.margins: 16; Layout.bottomMargin: 4; text: qsTr("Blocked"); font.bold: true }
                Repeater {
                    model: page.chats.blocked
                    delegate: PersonRow {
                        required property var model
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        chats: page.chats
                        mode: "blocked"
                        developer: page.backend.developerMode
                        userId: model.user
                        name: model.name
                    }
                }
                Rectangle { visible: page.chats.blocked.count > 0; Layout.fillWidth: true; height: 1; color: Theme.hairline; Layout.topMargin: 8 }
                Label { Layout.margins: 16; Layout.bottomMargin: 0; text: qsTr("Activity"); font.bold: true }
                Label {
                    visible: page.backend.error !== ""
                    Layout.fillWidth: true
                    Layout.margins: 16
                    wrapMode: Text.Wrap
                    color: Theme.danger
                    text: page.backend.error
                }
                ScrollList {
                    id: logView
                    Layout.fillWidth: true
                    // its own scrolling, in a box of its own height: the page scrolls past it
                    Layout.preferredHeight: Math.max(240, settingsScroll.height * 0.6)
                    Layout.margins: 12
                    model: page.backend.log
                    onCountChanged: positionViewAtEnd()
                    delegate: Label {
                        required property string display
                        width: ListView.view.width
                        text: display
                        wrapMode: Text.WrapAnywhere
                        font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
}
