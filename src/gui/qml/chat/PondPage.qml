import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Puddle.Files
import "../components"
import "../theme"

// A Pond's text channel, laid out like a conversation: each message under the name and time of who wrote it, following messages from the same
// person tucked under the first. Files go with messages as in a conversation (docs/ponds.md, "Files"). Our own messages can be edited
// (their words, and dropping files) and deleted, and anyone's by whoever may manage messages there; whoever may read the message
// log sees what was edited or deleted (docs/ponds.md, "Editing and deleting"). Messages can be replied to, quoting what they
// answer, and reacted to with an emoji; a message that mentions us is marked, and @ in the box names a member (or, for whoever
// may, a role or everyone) (docs/ponds.md, "Replies, reactions and mentions").
Pane {
    id: page

    required property var chats
    readonly property var ponds: chats.ponds
    property var video: null      // CallVideo: a voice channel's cameras and screens
    property var backend: null
    property var view: null       // the shell's video pane state
    property bool narrow: false
    property bool sidebarShown: true
    signal toggleSidebar()
    property bool contextShown: false
    signal toggleContext()

    padding: 0
    background: null

    readonly property bool canSend: ponds.channel !== "" && ponds.currentStatus !== "removed" && !muted && ponds.canSendHere
    // muted by an operator or administrator: until it runs out (looked at again each minute)
    property double now: Date.now()
    Timer { interval: 30000; running: page.ponds.mutedUntil >= 0; repeat: true; onTriggered: page.now = Date.now() }
    readonly property bool muted: ponds.mutedUntil === 0 || ponds.mutedUntil > now
    property var drafts: ({})
    property var people: null     // the Shell: someone's card and menu (showCard, personMenuAt)
    property string draftOwner: ""
    property string editingId: ""      // the message being edited in place
    property var editKeep: []          // while editing: the ids of the files it keeps
    // replying to (protocol 15)
    property string replyId: ""
    property string replyName: ""
    property string replyText: ""
    function startReply(m) {
        replyId = m.id; replyName = m.name || ""
        replyText = (m.shown || "").replace(/\s+/g, " ").slice(0, 100)
        input.forceActiveFocus()
    }
    function clearReply() { replyId = ""; replyName = ""; replyText = "" }
    // the message a reply quotes, brought into view if it is loaded
    function showMessage(id) {
        for (let i = 0; i < list.count; i++) {
            const m = ponds.messages.get(i)
            if (m && m.id === id) { list.stickToEnd = false; list.positionViewAtIndex(i, ListView.Center); flashId = id; flashTimer.restart(); return }
        }
    }
    property string flashId: ""
    Timer { id: flashTimer; interval: 1500; onTriggered: page.flashId = "" }
    // what @ named in the box: "@Name" -> "<@id>" (a member), "<@&id>" (a role); @everyone is written as it is
    property var picked: ({})
    function withMentions(t) {
        const names = Object.keys(picked).sort((a, b) => b.length - a.length)
        for (const n of names)
            t = t.split(n).join(picked[n])
        return t
    }
    function mention(item) {
        const at = mentionPopup.start, end = input.cursorPosition
        const label = "@" + item.name
        if (item.kind === "member") { const p = picked; p[label] = "<@" + item.id + ">"; picked = p }
        else if (item.kind === "role") { const p = picked; p[label] = "<@&" + item.id + ">"; picked = p }
        input.remove(at, end)
        input.insert(at, label + " ")
        mentionPopup.close()
    }
    EmojiPicker {
        id: emojiPicker
        objectName: "pondEmojiPicker"
        property string messageId: ""
        onPicked: emoji => page.ponds.toggleReaction(messageId, emoji)
    }
    function pickReaction(item, messageId) {
        emojiPicker.messageId = messageId
        const p = item.mapToItem(page, 0, item.height)
        emojiPicker.x = Math.max(8, Math.min(p.x - emojiPicker.width + item.width, page.width - emojiPicker.width - 8))
        emojiPicker.y = Math.min(p.y + 4, page.height - emojiPicker.height - 8)
        emojiPicker.open()
    }

    function startEdit(m) {
        editingId = m.id
        editKeep = (m.files || []).map(f => f.id)
    }
    // Up in an empty box edits your last message, as in a conversation
    function editLast() {
        if (!ponds.canEditMessages) return
        for (let i = list.count - 1; i >= 0; i--) {
            const m = ponds.messages.get(i)
            if (m && m.mine) { startEdit(m); return }
        }
    }

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
        const t = withMentions(input.text.trim())
        if (!canSend) return
        if (composer.count > 0) {          // files, with the words as their caption
            if (replyId !== "") { if (!composer.sendWith({ body: t, reply_to: Number(replyId) }, [])) return }
            else if (!composer.send(t, "")) return
            composer.caption = input.text.trim()
        } else {
            if (t === "") return
            ponds.send(t, replyId)
        }
        input.text = ""
        picked = ({})
        clearReply()
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
            page.editingId = ""
            page.clearReply()
            page.picked = ({})
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
                ToolButton {
                    objectName: "messageLogButton"
                    visible: page.ponds.canViewLogHere && page.ponds.channel !== ""
                    icon.source: Theme.icon("history"); icon.width: 20; icon.height: 20
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Message log")
                    ToolTip.text: qsTr("Message log: what was edited or deleted here"); ToolTip.visible: hovered
                    onClicked: messageLog.show("")
                }
                PanelToggle { glyph: "layout-sidebar-right"; label: qsTr("Members"); active: page.contextShown; onClicked: page.toggleContext() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        // the Pond cannot be reached, or no longer has us
        Rectangle {
            objectName: "pondStatus"
            readonly property string status: page.ponds.currentStatus
            visible: status === "unreachable" || status === "removed" || status === "not_proven"
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
                      : parent.status === "not_proven" ? qsTr("Something at %1's address answered without proving it's %1, so your Puddle isn't talking to it. Trying again…").arg(page.ponds.currentName)
                      : qsTr("Can't reach %1. Trying again…").arg(page.ponds.currentName)
            }
        }

        VideoPane {
            id: videoPane
            // in this channel's voice, and someone's camera or screen is on
            readonly property bool here: page.ponds.voiceState === "active" && page.ponds.voicePond === page.ponds.current &&
                                         page.ponds.voiceChannel === page.ponds.channel
            visible: here && page.video !== null && page.view !== null && !page.view.popped
            video: page.video
            view: page.view
            backend: page.backend
            chats: page.chats
            people: page.ponds.voicePeople.filter(id => id !== page.ponds.myId)
            nameOf: key => page.ponds.nameOf(key)
            avatarOf: key => page.ponds.avatarOf(key)
            selfPlace: "pond:" + page.ponds.current
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 8
            Layout.preferredHeight: page.view ? Math.max(160, page.height * page.view.share) : 0
            Layout.fillHeight: page.view !== null && page.view.full
        }
        // the split between the video and the messages: dragged to make either bigger
        Rectangle {
            objectName: "videoSplit"
            visible: videoPane.visible && !page.view.full
            Layout.fillWidth: true
            implicitHeight: 7
            color: splitDrag.containsMouse || splitDrag.pressed ? Theme.tint(Theme.text, Theme.hover) : "transparent"
            Rectangle { anchors.centerIn: parent; width: 40; height: 3; radius: 2; color: Theme.hairline }
            MouseArea {
                id: splitDrag
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SplitVCursor
                property real startY: 0
                property real startShare: 0
                onPressed: mouse => { startY = mapToItem(page, 0, mouse.y).y; startShare = page.view.share }
                onPositionChanged: mouse => {
                    if (pressed)
                        page.view.share = Math.max(0.2, Math.min(0.8, startShare + (mapToItem(page, 0, mouse.y).y - startY) / page.height))
                }
            }
        }

        ScrollList {
            id: list
            objectName: "pondMessageList"
            visible: !(videoPane.visible && page.view.full)
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
                readonly property bool editing: page.editingId === model.id
                readonly property bool mayEdit: page.ponds.canEditMessages && model.mine === true && page.ponds.currentStatus !== "removed"
                readonly property bool mayDelete: page.ponds.canEditMessages && (model.mine === true || page.ponds.canDeleteAnyHere) &&
                                                  page.ponds.currentStatus !== "removed"
                readonly property bool mayReply: page.ponds.canReply && page.canSend
                readonly property bool mayReact: page.ponds.canReactHere && page.ponds.currentStatus !== "removed" && !page.muted
                width: ListView.view.width
                implicitHeight: col.implicitHeight

                HoverHandler { id: hover }
                // on their name or picture, the menu about them; anywhere else, the message's
                ContextTrigger {
                    onRequested: (x, y) => {
                        for (const it of [msgAvatar, msgName]) {
                            const p = msg.mapToItem(it, x, y)
                            if (it.visible && it.contains(p) && page.people)
                                return page.people.personMenuAt(it, p.x, p.y, msg.model.author)
                        }
                        ctx.popup(msg, x, y)
                    }
                }
                Menu {
                    id: ctx
                    MenuItem {
                        text: bodyText.selectedText !== "" ? qsTr("Copy") : qsTr("Copy text")
                        onTriggered: bodyText.selectedText !== "" ? bodyText.copy() : page.chats.copy(msg.model.shown || msg.model.text)
                    }
                    MenuItem { objectName: "pondMsgReply"; text: qsTr("Reply"); visible: msg.mayReply; height: visible ? implicitHeight : 0; onTriggered: page.startReply(msg.model) }
                    MenuItem { objectName: "pondMsgReact"; text: qsTr("Add reaction"); visible: msg.mayReact; height: visible ? implicitHeight : 0; onTriggered: page.pickReaction(msgName.visible ? msgName : bodyText, msg.model.id) }
                    MenuItem { objectName: "pondMsgEdit"; text: qsTr("Edit"); visible: msg.mayEdit; height: visible ? implicitHeight : 0; onTriggered: page.startEdit(msg.model) }
                    MenuItem {
                        text: qsTr("Edit history")
                        visible: page.ponds.canViewLogHere && msg.model.edited > 0
                        height: visible ? implicitHeight : 0
                        onTriggered: messageLog.show(msg.model.id)
                    }
                    MenuSeparator { visible: msg.mayDelete; height: visible ? implicitHeight : 0 }
                    MenuItem {
                        objectName: "pondMsgDelete"
                        text: qsTr("Delete…"); palette.text: Theme.danger
                        visible: msg.mayDelete; height: visible ? implicitHeight : 0
                        onTriggered: page.askDelete(msg.model)
                    }
                }
                // under the day's line, when there is one: it marks the message, not the day
                Rectangle {
                    y: dayLine.visible ? dayLine.y + dayLine.height + 4 : 0
                    width: parent.width
                    height: parent.height - y
                    color: page.flashId === msg.model.id ? Theme.tint(Theme.accent, 0.18)
                           : msg.model.mentionsMe === true ? Theme.tint(Theme.accent, hover.hovered ? 0.14 : 0.09)
                           : hover.hovered || msg.editing ? Theme.tint(palette.text, 0.05) : "transparent"
                    // a message that mentions us: marked down its side, as on Discord
                    Rectangle { visible: msg.model.mentionsMe === true; width: 3; height: parent.height; color: Theme.accent; objectName: "pondMentionMark" }
                }

                ColumnLayout {
                    id: col
                    width: parent.width
                    spacing: 0

                    RowLayout {
                        id: dayLine
                        visible: msg.model.dayStart === true
                        Layout.fillWidth: true
                        Layout.margins: 16
                        Layout.bottomMargin: 4
                        spacing: 10
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                        Label { text: new Date(msg.model.ts).toLocaleDateString(Qt.locale(), Locale.LongFormat); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                    }

                    // what it replies to (protocol 15): clicked, that message is brought into view
                    RowLayout {
                        objectName: "pondReplyQuote"
                        visible: (msg.model.replyTo || "") !== ""
                        Layout.fillWidth: true
                        Layout.leftMargin: 72
                        Layout.rightMargin: 16
                        Layout.topMargin: 8
                        spacing: 6
                        Icon { glyph: "corner-up-left"; size: 14; opacity: 0.5 }
                        Label {
                            objectName: "pondReplyText"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            opacity: quoteHover.hovered ? 0.95 : 0.7
                            textFormat: Text.StyledText
                            text: msg.model.replyDeleted === true ? "<i>" + qsTr("Original message was deleted") + "</i>"
                                  : "<b>" + String(msg.model.replyName || "").replace(/&/g, "&amp;").replace(/</g, "&lt;") + "</b>  " +
                                    (msg.model.replyText ? String(msg.model.replyText).replace(/\s+/g, " ").replace(/&/g, "&amp;").replace(/</g, "&lt;")
                                                         : "<i>" + (msg.model.replyFiles > 0 ? qsTr("Click to see attachment") : "") + "</i>")
                            HoverHandler { id: quoteHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: page.showMessage(msg.model.replyTo) }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: msg.grouped ? 1 : ((msg.model.replyTo || "") !== "" ? 2 : 10)
                        Layout.bottomMargin: 1
                        spacing: 12

                        Item {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: msg.grouped ? 1 : 40
                            Layout.alignment: Qt.AlignTop
                            Avatar {
                                id: msgAvatar
                                objectName: "pondMessageAvatar"
                                visible: !msg.grouped; chats: page.chats; name: msg.model.name || ""; hash: msg.model.avatar || ""; tint: msg.model.tint || ""; size: 40
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: if (page.people) page.people.showCard(msgName, msg.model.author) }
                            }
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
                                Label {
                                    id: msgName
                                    objectName: "pondMessageName"
                                    text: msg.model.name || ""; font.bold: true; color: msg.model.nameColor ? msg.model.nameColor : palette.text /* their chosen colour, or their role's if they chose it (or the Pond says) */
                                    font.underline: nameHover.hovered
                                    HoverHandler { id: nameHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: if (page.people) page.people.showCard(msgName, msg.model.author) }
                                }
                                Label { text: page.dayLabel(msg.model.ts); font.pixelSize: 11; opacity: 0.55 }
                            }
                            TextEdit {
                                id: bodyText
                                objectName: "pondMessageText"
                                readonly property string plain: msg.model.shown || "" /* what it says, mentions as @names */
                                visible: (msg.model.text || "") !== "" && !msg.editing
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                wrapMode: TextEdit.Wrap
                                color: palette.text
                                font.pixelSize: 14
                                text: msg.model.html || ""          /* escaped, with mentions as @names (protocol 15) */
                                textFormat: TextEdit.RichText
                                Accessible.role: Accessible.StaticText
                                Accessible.name: msg.model.name + ": " + (msg.model.shown || "")
                            }
                            Label {
                                objectName: "pondMessageEdited"
                                visible: msg.model.edited > 0 && !msg.editing
                                text: qsTr("(edited)")
                                font.pixelSize: 10
                                opacity: 0.5
                                ToolTip.text: new Date(msg.model.edited).toLocaleString(Qt.locale(), Locale.ShortFormat)
                                ToolTip.visible: editedHover.hovered
                                HoverHandler { id: editedHover }
                            }
                            // editing in place: the words, and the files it keeps (an edit only drops them)
                            ColumnLayout {
                                visible: msg.editing
                                Layout.fillWidth: true
                                TextArea {
                                    id: editor
                                    objectName: "pondEditor"
                                    Layout.fillWidth: true
                                    wrapMode: TextArea.Wrap
                                    selectByMouse: true
                                    onTextChanged: if (length > 4000) remove(4000, length)
                                    text: msg.editing ? (msg.model.text || "") : ""
                                    onVisibleChanged: if (visible) { forceActiveFocus(); cursorPosition = length }
                                    FieldMenu {}
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Escape) { page.editingId = ""; event.accepted = true }
                                        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                                            page.saveEdit(msg.model, text)
                                            event.accepted = true
                                        }
                                    }
                                }
                                Flow {
                                    visible: !!msg.model.files && msg.model.files.length > 0
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Repeater {
                                        model: msg.editing ? (msg.model.files || []) : []
                                        delegate: Rectangle {
                                            required property var modelData
                                            readonly property bool kept: page.editKeep.indexOf(modelData.id) >= 0
                                            height: 28; radius: Theme.radius
                                            width: chip.implicitWidth + 12
                                            color: Theme.card; border.color: Theme.hairline
                                            opacity: kept ? 1 : 0.45
                                            RowLayout {
                                                id: chip
                                                anchors.centerIn: parent
                                                spacing: 4
                                                Label { text: modelData.name; font.strikeout: !parent.parent.kept; elide: Text.ElideMiddle; Layout.maximumWidth: 220 }
                                                ToolButton {
                                                    objectName: "pondEditDropFile"
                                                    icon.source: Theme.icon(parent.parent.kept ? "x" : "arrow-back-up"); icon.width: 14; icon.height: 14
                                                    display: AbstractButton.IconOnly
                                                    implicitWidth: 22; implicitHeight: 22
                                                    Accessible.name: parent.parent.kept ? qsTr("Take this file off") : qsTr("Keep this file")
                                                    ToolTip.text: Accessible.name; ToolTip.visible: hovered
                                                    onClicked: {
                                                        const id = modelData.id
                                                        page.editKeep = parent.parent.kept ? page.editKeep.filter(x => x !== id) : page.editKeep.concat([id])
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                Label { font.pixelSize: 11; opacity: 0.6; text: qsTr("Enter to save, Esc to cancel") }
                            }
                            MessageFiles {
                                visible: !!msg.model.files && msg.model.files.length > 0 && !msg.editing
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
                            // its reactions (protocol 15): ours marked; clicked, ours is added or taken back
                            Flow {
                                objectName: "pondReactions"
                                visible: !!msg.model.reactions && msg.model.reactions.length > 0 && !msg.editing
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 4
                                Repeater {
                                    model: msg.model.reactions || []
                                    delegate: Rectangle {
                                        id: chip
                                        required property var modelData
                                        objectName: "pondReaction"
                                        readonly property bool usable: msg.mayReact || modelData.mine
                                        height: 26; radius: 13
                                        width: chipRow.implicitWidth + 16
                                        color: modelData.mine ? Theme.tint(Theme.accent, 0.2) : Theme.tint(palette.text, chipHover.hovered ? 0.1 : 0.06)
                                        border.color: modelData.mine ? Theme.accent : "transparent"
                                        Accessible.role: Accessible.Button
                                        Accessible.name: qsTr("%1, %n reaction(s)", "", modelData.count).arg(modelData.emoji)
                                        Row {
                                            id: chipRow
                                            anchors.centerIn: parent
                                            spacing: 4
                                            Label { text: chip.modelData.emoji; font.pixelSize: 14 }
                                            Label { text: chip.modelData.count; font.pixelSize: 12; font.bold: chip.modelData.mine; anchors.verticalCenter: parent.verticalCenter }
                                        }
                                        HoverHandler { id: chipHover; cursorShape: chip.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                                        ToolTip.text: chip.modelData.names
                                        ToolTip.visible: chipHover.hovered
                                        ToolTip.delay: 400
                                        TapHandler { enabled: chip.usable; onTapped: page.ponds.toggleReaction(msg.model.id, chip.modelData.emoji) }
                                    }
                                }
                                ToolButton {
                                    objectName: "pondAddReaction"
                                    visible: msg.mayReact
                                    implicitWidth: 30; implicitHeight: 26
                                    icon.source: Theme.icon("mood-smile"); icon.width: 16; icon.height: 16
                                    display: AbstractButton.IconOnly
                                    opacity: hover.hovered ? 0.9 : 0.4
                                    Accessible.name: qsTr("Add reaction")
                                    ToolTip.text: Accessible.name; ToolTip.visible: hovered
                                    onClicked: page.pickReaction(this, msg.model.id)
                                }
                            }
                        }
                    }
                }

                // hover tools
                Row {
                    visible: hover.hovered && !msg.editing && (msg.mayEdit || msg.mayDelete || msg.mayReply || msg.mayReact)
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: (msg.model.dayStart === true ? 34 : 0) + ((msg.model.replyTo || "") !== "" ? 4 : 0)
                    Rectangle {
                        width: tools.implicitWidth + 4; height: 30; radius: Theme.radius
                        color: Theme.card; border.color: Theme.hairline
                        Row {
                            id: tools
                            anchors.centerIn: parent
                            ToolButton { id: reactButton; objectName: "pondReactButton"; visible: msg.mayReact; icon.source: Theme.icon("mood-smile"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Add reaction"); ToolTip.text: qsTr("Add reaction"); ToolTip.visible: hovered; onClicked: page.pickReaction(reactButton, msg.model.id) }
                            ToolButton { objectName: "pondReplyButton"; visible: msg.mayReply; icon.source: Theme.icon("arrow-back-up"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Reply"); ToolTip.text: qsTr("Reply"); ToolTip.visible: hovered; onClicked: page.startReply(msg.model) }
                            ToolButton { objectName: "pondEditButton"; visible: msg.mayEdit; icon.source: Theme.icon("pencil"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Edit"); ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered; onClicked: page.startEdit(msg.model) }
                            ToolButton { objectName: "pondDeleteButton"; visible: msg.mayDelete; icon.source: Theme.icon("trash"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Delete"); ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered; onClicked: page.askDelete(msg.model) }
                        }
                    }
                }
            }
        }

        // replying to (protocol 15)
        Rectangle {
            objectName: "pondReplyBar"
            visible: page.replyId !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 8
            implicitHeight: 30
            color: Theme.tint(palette.text, 0.08)
            radius: Theme.radius
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                Label { Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12; textFormat: Text.StyledText; text: qsTr("Replying to <b>%1</b>  %2").arg(page.replyName.replace(/&/g, "&amp;").replace(/</g, "&lt;")).arg(page.replyText.replace(/&/g, "&amp;").replace(/</g, "&lt;")) }
                ToolButton { objectName: "pondCancelReply"; icon.source: Theme.icon("x"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Cancel reply"); onClicked: page.clearReply() }
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
                                     : page.muted ? (page.ponds.mutedUntil > 0 ? qsTr("You're muted here until %1.").arg(new Date(page.ponds.mutedUntil).toLocaleTimeString(Qt.locale(), Locale.ShortFormat))
                                                                               : qsTr("You're muted here."))
                                     : !page.ponds.canSendHere ? qsTr("You can't send messages in #%1.").arg(page.ponds.channelName)
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
                    onCursorPositionChanged: mentionPopup.look()
                    Keys.onPressed: event => {
                        if (mentionPopup.opened && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                            mentionPopup.move(event.key === Qt.Key_Down ? 1 : -1)
                            event.accepted = true
                        } else if (mentionPopup.opened && (event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                            mentionPopup.choose()
                            event.accepted = true
                        } else if (mentionPopup.opened && event.key === Qt.Key_Escape) {
                            mentionPopup.close()
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                            page.send()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape && page.replyId !== "") {
                            page.clearReply()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up && text === "") {
                            page.editLast()
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
                enabled: page.canSend && page.ponds.canAttachHere && !composer.sending
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

    function saveEdit(m, text) {
        const t = text.trim(), had = (m.files || []).map(f => f.id)
        const dropped = had.some(id => editKeep.indexOf(id) < 0)
        if (t === "" && editKeep.length === 0) { editingId = ""; askDelete(m); return } // nothing left: that's deleting it
        if (t !== (m.text || "") || dropped) ponds.editMessage(m.id, t, editKeep)
        editingId = ""
    }
    function askDelete(m) {
        const theirs = !m.mine
        deleteConfirm.ask(qsTr("Delete message"),
                          theirs ? qsTr("Delete %1's message for everyone here? It stays in the message log, for those who may read it.").arg(m.name)
                                 : qsTr("Delete this message for everyone here?"),
                          qsTr("Delete"), () => page.ponds.deleteMessage(m.id))
    }
    // @ in the box: who it can name, as they type (protocol 15)
    Popup {
        id: mentionPopup
        objectName: "pondMentionPopup"
        property int start: -1          // where the @ is
        property var items: []
        property int current: 0
        x: box.x + 16
        y: box.y - height - 4
        width: Math.min(320, page.width - 32)
        padding: 4
        closePolicy: Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.card; border.color: Theme.hairline; radius: Theme.radius }
        // what comes before the cursor: @ and a word, at the start or after a space
        function look() {
            if (!page.ponds.canMention) { close(); return }
            const before = input.text.slice(0, input.cursorPosition)
            const m = before.match(/(^|\s)@([^\s@]{0,32})$/)
            if (!m) { close(); return }
            start = before.length - m[2].length - 1
            items = page.ponds.mentionables(m[2])
            current = 0
            if (items.length > 0) open(); else close()
        }
        function move(by) { current = (current + by + items.length) % items.length }
        function choose() { if (items.length > 0) page.mention(items[current]) }
        contentItem: ListView {
            implicitHeight: contentHeight
            interactive: false
            model: mentionPopup.items
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                objectName: "pondMentionChoice"
                width: ListView.view.width
                highlighted: index === mentionPopup.current
                contentItem: RowLayout {
                    spacing: 8
                    Avatar { visible: modelData.kind === "member"; chats: page.chats; name: modelData.name; hash: modelData.avatar || ""; tint: modelData.color || ""; size: 22 }
                    Icon { visible: modelData.kind !== "member"; glyph: "users"; size: 18; opacity: 0.7 }
                    Label { text: (modelData.kind === "member" ? "" : "@") + modelData.name; Layout.fillWidth: true; elide: Text.ElideRight; color: modelData.kind === "role" && modelData.color ? modelData.color : palette.text }
                    Label { visible: modelData.kind === "everyone"; text: qsTr("everyone who sees this channel"); opacity: 0.55; font.pixelSize: 11 }
                }
                onClicked: page.mention(modelData)
            }
        }
    }
    ConfirmDialog { id: deleteConfirm; objectName: "pondDeleteConfirm" }
    MessageLog {
        id: messageLog
        ponds: page.ponds
        chats: page.chats
        onSaveFile: f => fileActions.save(f)
        onOpenFile: f => fileActions.open(f)
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
