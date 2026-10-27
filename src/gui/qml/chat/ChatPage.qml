import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A direct message, laid out like Discord: no bubbles, each message a line of text under the name and time of who wrote it,
// following messages from the same person tucked under the first. Hover for reply, edit and delete.
Pane {
    id: page

    required property var chats
    property bool narrow: false
    property bool sidebarShown: true    // the conversations card is open
    signal toggleSidebar()
    property bool contextShown: false   // the side panel is open
    signal toggleContext()

    padding: 0
    background: null   // the conversation sits straight on the window's canvas

    readonly property var messages: chats.messages
    readonly property bool canSend: chats.currentState === "active"
    property string replyId: ""
    property string replyName: ""
    property string replyText: ""
    property string editingId: ""
    property var drafts: ({})
    property string draftOwner: ""

    readonly property string timeFormat: Qt.locale().timeFormat(Locale.ShortFormat).replace(/[:.]ss/, "")
    function clock(d) { return Qt.formatTime(d, timeFormat) }
    function dayLabel(ts) {
        const d = new Date(ts), now = new Date()
        const midnight = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime()
        const days = Math.round((midnight(now) - midnight(d)) / 86400000)
        const time = clock(d)
        if (days === 0) return qsTr("Today at %1").arg(time)
        if (days === 1) return qsTr("Yesterday at %1").arg(time)
        return Qt.formatDate(d, Locale.ShortFormat) + " " + time
    }
    function dividerLabel(ts) {
        return new Date(ts).toLocaleDateString(Qt.locale(), Locale.LongFormat)
    }
    // In a group the delivery reported is the worst of what became of it for each member, so "not delivered yet" means someone is away: say that,
    // not that "the group" cannot be reached. A first try gets a few seconds before it is called waiting.
    property real nowMs: Date.now()
    Timer { interval: 2000; running: page.visible && page.chats.currentIsGroup; repeat: true; onTriggered: page.nowMs = Date.now() }
    function waitingOnGroup(m) {
        if (!page.chats.currentIsGroup) return false
        return m.deliveryState === "retrying" || (m.deliveryState === "queued" && page.nowMs - m.ts > 6000)
    }
    function startReply(m) {
        replyId = m.id; replyName = m.senderName; replyText = m.body ? m.body.replace(/\s+/g, " ").slice(0, 100) : ""
        input.forceActiveFocus()
    }
    function clearReply() { replyId = ""; replyName = ""; replyText = "" }
    function send() {
        const t = input.text.trim()
        if (t === "" || !canSend) return
        chats.send(t, replyId)
        input.text = ""
        clearReply()
        list.stickToEnd = true
    }
    // Up in an empty box edits your last message, like Discord.
    function editLast() {
        for (let i = messages.count - 1; i >= 0; i--) {
            const m = chats.messageAt(i)
            if (m.outgoing && !m.deleted) { editingId = m.id; return }
        }
    }

    // each chat keeps its own unsent text
    Connections {
        target: page.chats
        function onCurrentChanged() {
            const d = page.drafts
            if (page.draftOwner !== "") d[page.draftOwner] = input.text
            page.draftOwner = page.chats.current
            input.text = d[page.draftOwner] || ""
            page.drafts = d
            page.clearReply()
            page.editingId = ""
            list.stickToEnd = true
            if (page.chats.current !== "") input.forceActiveFocus()
        }
    }

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
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10
                PanelToggle { objectName: "sidebarButton"; glyph: "layout-sidebar"; label: qsTr("Conversations"); active: page.sidebarShown; onClicked: page.toggleSidebar() }
                Avatar { chats: page.chats; name: page.chats.currentName; hash: page.chats.currentAvatar; tint: page.chats.currentColor; size: 28 }
                Label { text: page.chats.currentName; font.bold: true; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                PanelToggle {
                    objectName: "contextButton"
                    visible: page.chats.current !== ""
                    glyph: "layout-sidebar-right"
                    label: page.chats.currentIsGroup ? qsTr("Members") : qsTr("Profile")
                    active: page.contextShown
                    onClicked: page.toggleContext()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        // the messages
        ScrollList {
            id: list
            objectName: "messageList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: page.messages
            property bool stickToEnd: true
            // (the list starts at originY, which is negative while the "loading earlier messages" header is there)
            readonly property bool atBottom: contentY + height >= originY + contentHeight - 4

            onCountChanged: if (stickToEnd) Qt.callLater(positionViewAtEnd)
            onContentHeightChanged: {
                if (stickToEnd) Qt.callLater(positionViewAtEnd)
                fillTimer.restart()
            }
            // too short to scroll, so scrolling cannot ask for more: fill the window. (Looked at once things have settled: while rows are being
            // measured the list's height is an estimate, and can briefly say "short" when it is not.)
            Timer {
                id: fillTimer
                interval: 300
                onTriggered: if (list.contentHeight <= list.height && page.chats.hasOlder) page.chats.loadOlder()
            }
            // scrolling away from the end stops following; reaching the end follows again
            onUserScrolled: {
                stickToEnd = atBottom
                // ask for the page before once you are within a screenful of the top (and not at the top itself: where that is moves as rows are measured)
                if (contentY - originY < height && page.chats.hasOlder) page.chats.loadOlder()
            }

            header: Item {
                width: list.width
                height: page.chats.hasOlder ? 40 : 0
                Label { anchors.centerIn: parent; visible: page.chats.hasOlder; opacity: 0.55; text: qsTr("Loading earlier messages…") }
            }

            delegate: Item {
                id: msg
                required property var model
                readonly property bool mine: model.outgoing === true
                readonly property bool grouped: model.grouped === true
                readonly property bool notice: model.kind === "notice"
                readonly property bool editing: page.editingId === model.id
                width: ListView.view.width
                implicitHeight: col.implicitHeight

                HoverHandler { id: hover }
                ContextTrigger { active: !msg.notice; onRequested: (x, y) => ctx.popup(msg, x, y) }

                Menu {
                    id: ctx
                    // with some words selected it copies those, otherwise the whole message
                    MenuItem {
                        objectName: "msgCopy"
                        text: bodyText.selectedText !== "" ? qsTr("Copy") : qsTr("Copy text")
                        enabled: bodyText.selectedText !== "" || !!msg.model.body
                        onTriggered: bodyText.selectedText !== "" ? bodyText.copy() : page.chats.copy(msg.model.body)
                    }
                    MenuItem { objectName: "msgReply"; text: qsTr("Reply"); enabled: !msg.model.deleted; onTriggered: page.startReply(msg.model) }
                    MenuItem { objectName: "msgEdit"; text: qsTr("Edit"); visible: msg.mine && !msg.model.deleted; height: visible ? implicitHeight : 0; onTriggered: page.editingId = msg.model.id }
                    MenuSeparator { visible: msg.mine && !msg.model.deleted; height: visible ? implicitHeight : 0 }
                    MenuItem { objectName: "msgDelete"; text: qsTr("Delete…"); palette.text: Theme.danger; visible: msg.mine && !msg.model.deleted; height: visible ? implicitHeight : 0; onTriggered: confirm.ask(msg.model.id) }
                }

                Rectangle {
                    anchors.fill: parent
                    color: hover.hovered || msg.editing ? Theme.tint(palette.text, 0.05) : "transparent"
                }

                ColumnLayout {
                    id: col
                    width: parent.width
                    spacing: 0

                    // a new day
                    RowLayout {
                        visible: msg.model.dayStart === true
                        Layout.fillWidth: true
                        Layout.margins: 16
                        Layout.bottomMargin: 4
                        spacing: 10
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                        Label { text: page.dividerLabel(msg.model.ts); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                    }

                    // what it replies to
                    RowLayout {
                        visible: !msg.notice && msg.model.reply_to !== undefined
                        Layout.leftMargin: 72
                        Layout.rightMargin: 16
                        Layout.topMargin: 6
                        spacing: 6
                        Icon { glyph: "corner-up-left"; size: 14; opacity: 0.5 }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            opacity: 0.7
                            text: msg.model.replyMissing === true ? qsTr("An earlier message")
                                  : "<b>" + (msg.model.replyName || "") + "</b>  " + (msg.model.replyText === "" ? "<i>" + qsTr("Message deleted") + "</i>" : String(msg.model.replyText).replace(/&/g, "&amp;").replace(/</g, "&lt;"))
                            textFormat: Text.StyledText
                        }
                    }

                    // something that happened to the group, between the messages
                    Label {
                        objectName: "groupNotice"
                        visible: msg.notice
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: 6
                        Layout.bottomMargin: 6
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        opacity: 0.6
                        text: msg.model.noticeText || ""
                    }

                    RowLayout {
                        visible: !msg.notice
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: msg.grouped ? 1 : 10
                        Layout.bottomMargin: 1
                        spacing: 12

                        // the picture sits with the first line; later lines show the time on hover
                        Item {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: msg.grouped ? 1 : 40
                            Layout.alignment: Qt.AlignTop
                            Avatar {
                                visible: !msg.grouped
                                chats: page.chats; name: msg.model.senderName || ""; hash: msg.model.avatar || ""; tint: msg.model.tint || ""; size: 40
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
                                Label { text: msg.model.senderName || ""; font.bold: true }
                                Label { text: page.dayLabel(msg.model.ts); font.pixelSize: 11; opacity: 0.55 }
                            }
                            TextEdit {
                                id: bodyText
                                objectName: "messageText"
                                visible: !msg.editing
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true // the selection stays while the menu is open
                                wrapMode: TextEdit.Wrap
                                color: palette.text
                                font.pixelSize: 14
                                opacity: msg.model.deleted ? 0.5 : 1
                                font.italic: msg.model.deleted === true
                                text: msg.model.deleted ? qsTr("Message deleted") : (msg.model.body || "") + (msg.model.edited ? "" : "")
                                textFormat: TextEdit.PlainText
                                Accessible.role: Accessible.StaticText
                                Accessible.name: msg.model.senderName + ": " + text
                            }
                            // what became of it, when it has not been delivered yet
                            RowLayout {
                                objectName: "deliveryNote"
                                visible: msg.mine && !!msg.model.deliveryState && !msg.model.deleted
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 11
                                    color: msg.model.deliveryState === "failed" ? Theme.danger : msg.model.deliveryState === "held" ? Theme.warning : palette.text
                                    opacity: msg.model.deliveryState === "failed" || msg.model.deliveryState === "held" ? 1 : 0.6
                                    text: page.waitingOnGroup(msg.model) ? qsTr("Waiting for group members to come online. It will be delivered when they do.")
                                          : msg.model.deliveryState === "queued" ? qsTr("Sending…")
                                          : msg.model.deliveryState === "retrying" ? qsTr("%1 can't be reached right now. Trying again at %2.").arg(page.chats.currentName).arg(page.clock(new Date(msg.model.deliveryNext)))
                                          : msg.model.deliveryState === "failed" ? qsTr("Not delivered: %1").arg(msg.model.deliveryReason || qsTr("unknown reason"))
                                          : msg.model.deliveryState === "held" ? qsTr("Not sent: %1's certificate changed.").arg(page.chats.currentName)
                                          : ""
                                }
                                Button {
                                    objectName: "retryButton"
                                    visible: msg.model.deliveryState === "failed" || msg.model.deliveryState === "retrying"
                                    text: msg.model.deliveryState === "failed" ? qsTr("Retry") : qsTr("Try now")
                                    onClicked: page.chats.retry()
                                }
                            }
                            Label {
                                visible: msg.model.edited === true && !msg.model.deleted && !msg.editing
                                text: qsTr("(edited)")
                                font.pixelSize: 10
                                opacity: 0.5
                            }
                            // editing in place
                            ColumnLayout {
                                visible: msg.editing
                                Layout.fillWidth: true
                                TextArea {
                                    id: editor
                    objectName: "editor"
                                    Layout.fillWidth: true
                                    wrapMode: TextArea.Wrap
                                    selectByMouse: true
                                    onTextChanged: if (length > 4000) remove(4000, length)
                                    text: msg.editing ? (msg.model.body || "") : ""
                                    onVisibleChanged: if (visible) { forceActiveFocus(); cursorPosition = length }
                                    FieldMenu {}
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Escape) { page.editingId = ""; event.accepted = true }
                                        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                                            if (text.trim() !== "" && text.trim() !== msg.model.body) page.chats.edit(msg.model.id, text)
                                            page.editingId = ""
                                            event.accepted = true
                                        }
                                    }
                                }
                                Label { font.pixelSize: 11; opacity: 0.6; text: qsTr("Enter to save, Esc to cancel") }
                            }
                        }
                    }
                }

                // hover tools
                Row {
                    visible: hover.hovered && !msg.notice && !msg.editing && !msg.model.deleted
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: msg.model.dayStart === true ? 34 : 0
                    spacing: 0
                    Rectangle {
                        width: tools.implicitWidth + 4; height: 30; radius: Theme.radius
                        color: Theme.card; border.color: Theme.hairline
                        Row {
                            id: tools
                            anchors.centerIn: parent
                            ToolButton { objectName: "replyButton"; icon.source: Theme.icon("arrow-back-up"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Reply"); ToolTip.text: qsTr("Reply"); ToolTip.visible: hovered; onClicked: page.startReply(msg.model) }
                            ToolButton { objectName: "editButton"; visible: msg.mine; icon.source: Theme.icon("pencil"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Edit"); ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered; onClicked: page.editingId = msg.model.id }
                            ToolButton { objectName: "deleteButton"; visible: msg.mine; icon.source: Theme.icon("trash"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Delete"); ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered; onClicked: confirm.ask(msg.model.id) }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.55
                visible: list.count === 0
                text: qsTr("This is the start of your conversation with %1.").arg(page.chats.currentName)
            }
        }

        // jump back down after scrolling up
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 0
            Button {
                visible: !list.atBottom && list.count > 0
                anchors.right: parent.right
                anchors.rightMargin: 24
                y: -44
                text: qsTr("Latest")
                icon.source: Theme.icon("arrow-down")
                icon.width: 16
                icon.height: 16
                onClicked: { list.stickToEnd = true; list.positionViewAtEnd() }
            }
        }

        // their certificate changed: nothing can be sent until the owner decides
        Rectangle {
            visible: page.chats.currentCertChanged
            Layout.fillWidth: true
            implicitHeight: certRow.implicitHeight + 16
            color: Qt.rgba(0.85, 0.63, 0, 0.15)
            RowLayout {
                id: certRow
                anchors.fill: parent
                anchors.margins: 8
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("%1's certificate changed. Nothing is sent until you trust it.").arg(page.chats.currentName) }
                Button { text: qsTr("Trust"); onClicked: page.chats.trust(page.chats.current) }
            }
        }

        // replying to
        Rectangle {
            visible: page.replyId !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            implicitHeight: 30
            color: Theme.tint(palette.text, 0.08)
            radius: Theme.radius
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                Label { Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12; textFormat: Text.StyledText; text: qsTr("Replying to <b>%1</b>  %2").arg(page.replyName).arg(page.replyText.replace(/</g, "&lt;")) }
                ToolButton { icon.source: Theme.icon("x"); icon.width: Theme.iconSize; icon.height: Theme.iconSize; display: AbstractButton.IconOnly; Accessible.name: qsTr("Cancel reply"); onClicked: page.clearReply() }
            }
        }

        // the box: one line is a pill; it grows with the text. The send button sits in its bottom right corner, the same distance from the top, the
        // bottom and the right edge as the text is from the top and bottom.
        Rectangle {
            id: box
            readonly property int pad: 6          // from the edge to the button
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
                    objectName: "composer"
                    enabled: page.canSend
                    placeholderText: page.canSend ? qsTr("Message %1").arg(page.chats.currentName)
                                     : page.chats.currentState === "closed" ? qsTr("This stream is closed. You can read the history.")
                                     : qsTr("You can write once %1 has accepted.").arg(page.chats.currentName)
                    wrapMode: TextArea.Wrap
                    selectByMouse: true
                    color: Theme.text
                    // the same above and below a single line as beside the button; the text keeps clear of the button on the right
                    leftPadding: 16
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
                        } else if (event.key === Qt.Key_Up && text === "") {
                            page.editLast()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape && page.replyId !== "") {
                            page.clearReply()
                            event.accepted = true
                        }
                    }
                }
            }

            Rectangle {
                id: sendButton
                objectName: "sendButton"
                readonly property bool ready: page.canSend && input.text.trim() !== ""
                // a long message brings a scroll bar to the right edge (as in Discord): the button makes room for it
                readonly property bool scrolls: input.implicitHeight > scroller.height
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: box.pad
                anchors.rightMargin: box.pad + (scrolls ? 10 : 0)
                Behavior on anchors.rightMargin { NumberAnimation { duration: 100 } }
                width: box.buttonSize
                height: box.buttonSize
                radius: width / 2
                color: !ready ? Theme.tint(Theme.text, Theme.hover) : sendTap.pressed ? Theme.primaryPressed : Theme.primary
                Behavior on color { ColorAnimation { duration: 90 } }
                activeFocusOnTab: ready
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Send")
                Accessible.onPressAction: page.send()
                Keys.onSpacePressed: page.send()
                Keys.onReturnPressed: page.send()
                Icon { anchors.centerIn: parent; glyph: "arrow-up"; size: 18; color: sendButton.ready ? "white" : Theme.tint(Theme.text, 0.4) }
                Rectangle { anchors.fill: parent; anchors.margins: -3; radius: width / 2; color: "transparent"; border.width: 2; border.color: Theme.accent; visible: sendButton.activeFocus }
                HoverHandler { cursorShape: sendButton.ready ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler {
                    id: sendTap
                    enabled: sendButton.ready
                    onTapped: { page.send(); input.forceActiveFocus() }
                }
            }
        }
    }

    Dialog {
        id: confirm
        property string target: ""
        function ask(id) { target = id; open() }
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Delete message")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("Delete this message for both of you?") }
        onAccepted: page.chats.remove(target)
    }
}
