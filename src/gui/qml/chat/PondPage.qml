import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Puddle.Files
import "../components"
import "../theme"

// A Pond's text channel, laid out like a conversation: each message under the name and time of who wrote it, following messages from the same
// person tucked under the first. Files go with messages as in a conversation (docs/ponds.md, "Files"); no replies or edits yet.
Pane {
    id: page

    required property var chats
    readonly property var ponds: chats.ponds
    property bool narrow: false
    property bool sidebarShown: true
    signal toggleSidebar()
    property bool contextShown: false
    signal toggleContext()

    padding: 0
    background: null

    readonly property bool canSend: ponds.channel !== "" && ponds.currentStatus !== "removed"
    property var drafts: ({})
    property string draftOwner: ""

    readonly property string timeFormat: Qt.locale().timeFormat(Locale.ShortFormat).replace(/[:.]ss/, "")
    function clock(d) { return Qt.formatTime(d, timeFormat) }
    function dayLabel(ts) {
        const d = new Date(ts), now = new Date()
        const midnight = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime()
        const days = Math.round((midnight(now) - midnight(d)) / 86400000)
        if (days === 0) return qsTr("Today at %1").arg(clock(d))
        if (days === 1) return qsTr("Yesterday at %1").arg(clock(d))
        return Qt.formatDate(d, Locale.ShortFormat) + " " + clock(d)
    }
    function send() {
        const t = input.text.trim()
        if (!canSend) return
        if (composer.count > 0) {          // files, with the words as their caption
            if (!composer.send(t, "")) return
            composer.caption = t
        } else {
            if (t === "") return
            ponds.send(t)
        }
        input.text = ""
        list.stickToEnd = true
    }

    // the files waiting to go with the next message: chosen with the paperclip, dropped on the page, or pasted
    Composer {
        id: composer
        property string caption: ""        // the words that went with them, given back if they could not be sent
        chats: page.chats
        target: page.ponds.channel !== "" ? page.ponds.sendPath() : ""
        onRefused: why => page.chats.failed(why)
        onFailed: why => {
            page.chats.failed(why)
            if (input.text === "") input.text = caption
        }
        onSent: { caption = ""; page.ponds.refresh() }
    }
    FileActions {
        id: fileActions
        chats: page.chats
        onFailed: why => page.chats.failed(why)
    }
    FileDialog {
        id: picker
        title: qsTr("Choose files to send")
        fileMode: FileDialog.OpenFiles
        onAccepted: composer.add(selectedFiles)
    }

    // each channel keeps its own unsent text
    Connections {
        target: page.ponds
        function onChannelChanged() {
            const key = page.ponds.current + "/" + page.ponds.channel
            if (key === page.draftOwner) return
            const d = page.drafts
            if (page.draftOwner !== "") d[page.draftOwner] = input.text
            page.draftOwner = key
            input.text = d[key] || ""
            page.drafts = d
            list.stickToEnd = true
            if (page.visible && page.ponds.channel !== "") input.forceActiveFocus()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                PanelToggle { glyph: "layout-sidebar"; label: qsTr("Channels"); active: page.sidebarShown; onClicked: page.toggleSidebar() }
                Icon { glyph: "hash"; size: 20; opacity: 0.6; visible: page.ponds.channel !== "" }
                Label { objectName: "channelTitle"; text: page.ponds.channelName; font.bold: true; font.pixelSize: 16 }
                Rectangle { visible: page.ponds.channelTopic !== ""; width: 1; height: 20; color: Theme.hairline }
                Label { text: page.ponds.channelTopic; opacity: 0.6; elide: Text.ElideRight; Layout.fillWidth: true }
                Item { Layout.fillWidth: page.ponds.channelTopic === "" }
                PanelToggle { glyph: "layout-sidebar-right"; label: qsTr("Members"); active: page.contextShown; onClicked: page.toggleContext() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        // the Pond cannot be reached, or no longer has us
        Rectangle {
            objectName: "pondStatus"
            readonly property string status: page.ponds.currentStatus
            visible: status === "unreachable" || status === "removed" || status === "certificate_changed"
            Layout.fillWidth: true
            implicitHeight: statusLabel.implicitHeight + 12
            color: status === "removed" ? Theme.danger : Theme.banner
            Label {
                id: statusLabel
                anchors.centerIn: parent
                width: parent.width - 24
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: "white"
                text: parent.status === "removed" ? qsTr("You are no longer a member of %1. You can still read what was kept.").arg(page.ponds.currentName)
                      : parent.status === "certificate_changed" ? qsTr("%1's certificate changed, so your Puddle has stopped talking to it.").arg(page.ponds.currentName)
                      : qsTr("Can't reach %1. Trying again…").arg(page.ponds.currentName)
            }
        }

        ScrollList {
            id: list
            objectName: "pondMessageList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: page.ponds.messages
            property bool stickToEnd: true
            readonly property bool atBottom: contentY + height >= originY + contentHeight - 4

            onCountChanged: if (stickToEnd) Qt.callLater(positionViewAtEnd)
            onContentHeightChanged: {
                if (stickToEnd) Qt.callLater(positionViewAtEnd)
                fillTimer.restart()
            }
            Timer {
                id: fillTimer
                interval: 300
                onTriggered: if (list.contentHeight <= list.height && page.ponds.hasOlder) page.ponds.loadOlder()
            }
            onUserScrolled: {
                stickToEnd = atBottom
                if (contentY - originY < height && page.ponds.hasOlder) page.ponds.loadOlder()
            }

            header: Item {
                width: list.width
                height: page.ponds.hasOlder ? 40 : (page.ponds.channel !== "" ? welcome.implicitHeight + 32 : 0)
                Label { anchors.centerIn: parent; visible: page.ponds.hasOlder; opacity: 0.55; text: qsTr("Loading earlier messages…") }
                // the start of the channel
                ColumnLayout {
                    id: welcome
                    visible: !page.ponds.hasOlder && page.ponds.channel !== ""
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.margins: 16
                    spacing: 4
                    Label { text: qsTr("Welcome to #%1").arg(page.ponds.channelName); font.bold: true; font.pixelSize: 22 }
                    Label { text: qsTr("This is the start of the #%1 channel.").arg(page.ponds.channelName); opacity: 0.6 }
                }
            }
            footer: Item {
                width: list.width
                height: lineHeight.height
                FontMetrics { id: lineHeight }
            }

            delegate: Item {
                id: msg
                required property var model
                readonly property bool grouped: model.grouped === true
                width: ListView.view.width
                implicitHeight: col.implicitHeight

                HoverHandler { id: hover }
                ContextTrigger { onRequested: (x, y) => ctx.popup(msg, x, y) }
                Menu {
                    id: ctx
                    MenuItem {
                        text: bodyText.selectedText !== "" ? qsTr("Copy") : qsTr("Copy text")
                        onTriggered: bodyText.selectedText !== "" ? bodyText.copy() : page.chats.copy(msg.model.text)
                    }
                }
                Rectangle { anchors.fill: parent; color: hover.hovered ? Theme.tint(palette.text, 0.05) : "transparent" }

                ColumnLayout {
                    id: col
                    width: parent.width
                    spacing: 0

                    RowLayout {
                        visible: msg.model.dayStart === true
                        Layout.fillWidth: true
                        Layout.margins: 16
                        Layout.bottomMargin: 4
                        spacing: 10
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                        Label { text: new Date(msg.model.ts).toLocaleDateString(Qt.locale(), Locale.LongFormat); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: msg.grouped ? 1 : 10
                        Layout.bottomMargin: 1
                        spacing: 12

                        Item {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: msg.grouped ? 1 : 40
                            Layout.alignment: Qt.AlignTop
                            Avatar { visible: !msg.grouped; name: msg.model.name || ""; tint: msg.model.tint || ""; size: 40 }
                            Label {
                                visible: msg.grouped && hover.hovered
                                anchors.horizontalCenter: parent.horizontalCenter
                                font.pixelSize: 10
                                opacity: 0.6
                                text: page.clock(new Date(msg.model.ts))
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                visible: !msg.grouped
                                spacing: 8
                                Label { text: msg.model.name || ""; font.bold: true; color: msg.model.tint && !msg.model.mine ? msg.model.tint : palette.text }
                                Label { text: page.dayLabel(msg.model.ts); font.pixelSize: 11; opacity: 0.55 }
                            }
                            TextEdit {
                                id: bodyText
                                objectName: "pondMessageText"
                                visible: text !== ""
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                wrapMode: TextEdit.Wrap
                                color: palette.text
                                font.pixelSize: 14
                                text: msg.model.text || ""
                                textFormat: TextEdit.PlainText
                                Accessible.role: Accessible.StaticText
                                Accessible.name: msg.model.name + ": " + text
                            }
                            MessageFiles {
                                visible: !!msg.model.files && msg.model.files.length > 0
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                chats: page.chats
                                fetcher: page.ponds
                                files: msg.model.files || []
                                messageId: msg.model.id
                                onViewMedia: (list, i) => fileActions.view(list, i)
                                onSaveFile: f => fileActions.save(f)
                                onOpenFile: f => fileActions.open(f)
                            }
                        }
                    }
                }
            }
        }

        // the files that go with the next message
        AttachmentTray {
            composer: composer
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 8
        }

        // the box: as in a conversation
        Rectangle {
            id: box
            readonly property int pad: 6
            readonly property int buttonSize: 32
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.topMargin: 8
            implicitHeight: Math.min(Math.max(input.implicitHeight, buttonSize + 2 * pad), 160)
            radius: Theme.buttonRadius
            color: Theme.card
            border.width: 1
            border.color: input.activeFocus ? Theme.accent : Theme.hairline
            opacity: page.canSend ? 1 : 0.6

            ScrollView {
                id: scroller
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                TextArea {
                    id: input
                    objectName: "pondComposer"
                    enabled: page.canSend
                    placeholderText: page.ponds.currentStatus === "removed" ? qsTr("You are no longer a member here.")
                                     : qsTr("Message #%1").arg(page.ponds.channelName)
                    wrapMode: TextArea.Wrap
                    selectByMouse: true
                    color: Theme.text
                    leftPadding: box.pad + box.buttonSize + 8
                    rightPadding: box.pad * 2 + box.buttonSize + 10
                    topPadding: Math.max(6, Math.round((box.buttonSize + 2 * box.pad - fontMetrics.height) / 2))
                    bottomPadding: topPadding
                    onTextChanged: if (length > 4000) remove(4000, length)
                    background: null
                    FontMetrics { id: fontMetrics; font: input.font }
                    FieldMenu {}
                    Accessible.name: qsTr("Message")
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                            page.send()
                            event.accepted = true
                        } else if (event.matches(StandardKey.Paste) && composer.canPaste()) { // a picture or files: they go with the message
                            composer.paste()
                            event.accepted = true
                        }
                    }
                }
            }

            ToolButton {
                objectName: "pondAttachButton"
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: box.pad
                width: box.buttonSize
                height: box.buttonSize
                enabled: page.canSend && !composer.sending
                icon.source: Theme.icon("paperclip"); icon.width: 18; icon.height: 18
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Add files")
                ToolTip.text: qsTr("Add files"); ToolTip.visible: hovered
                onClicked: picker.open()
            }

            Rectangle {
                id: sendButton
                objectName: "pondSendButton"
                readonly property bool ready: page.canSend && !composer.sending && (input.text.trim() !== "" || composer.ready)
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: box.pad
                width: box.buttonSize
                height: box.buttonSize
                radius: width / 2
                color: !ready ? Theme.tint(Theme.text, Theme.hover) : sendTap.pressed ? Theme.primaryPressed : Theme.primary
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Send")
                Accessible.onPressAction: page.send()
                Icon { anchors.centerIn: parent; glyph: "arrow-up"; size: 18; color: sendButton.ready ? "white" : Theme.tint(Theme.text, 0.4) }
                HoverHandler { cursorShape: sendButton.ready ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler {
                    id: sendTap
                    enabled: sendButton.ready
                    onTapped: { page.send(); input.forceActiveFocus() }
                }
            }
        }
    }

    // files dropped anywhere on the channel go with the next message
    DropArea {
        id: dropArea
        anchors.fill: parent
        enabled: page.canSend && !composer.sending
        onEntered: drag => { if (!drag.hasUrls) drag.accepted = false }
        onDropped: drop => {
            composer.add(drop.urls)
            drop.accept(Qt.CopyAction)
            input.forceActiveFocus()
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            visible: dropArea.containsDrag
            radius: Theme.radiusLarge
            color: Theme.tint(Theme.accent, 0.12)
            border.width: 2
            border.color: Theme.accent
            Label { anchors.centerIn: parent; text: qsTr("Drop to add to your message"); font.pixelSize: 16; font.bold: true }
        }
    }
}
