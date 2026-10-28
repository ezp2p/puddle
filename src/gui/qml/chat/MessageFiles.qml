import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import "../components"
import "../theme"

// The files of one message (docs/files.md): photos and videos as a grid, everything else as a card each. Until a file is
// here, its sender's tiny preview stands in for it, blurred, with what is happening to it; one over the size the Puddle
// fetches by itself has a Download button. Clicking a photo or a video opens it (viewMedia); a file is saved or opened.
// Under them, lines for what passed between Puddles: who would not take a file as too large, and who asks for one again.
ColumnLayout {
    id: root

    required property var chats
    required property var files
    required property string messageId
    signal viewMedia(var list, int index)
    signal saveFile(var file)
    signal openFile(var file)

    // Whoever has it would not send it again unasked: they sent it whole once already (docs/files.md, "Asking again").
    function refusedToUs(f) { return (f.state === "downloading" || f.state === "failed") && f.reason === "they do not have it" && !f.asked }
    // What passed between Puddles over these files, said as lines under the message: who would not take one, who asks again.
    readonly property var notes: {
        const out = []
        for (const f of (files || [])) {
            for (const r of (f.refused || []))
                out.push({ kind: "refused", f: f, user: r.user, max: r.max })
            for (const u of (f.requests || []))
                out.push({ kind: "request", f: f, user: u })
        }
        return out
    }
    function isMedia(f) { return f.mime.startsWith("image/") || f.mime.startsWith("video/") }
    readonly property var media: (files || []).filter(f => isMedia(f))
    readonly property var others: (files || []).filter(f => !isMedia(f))
    function clock(ms) {
        const s = Math.round(ms / 1000)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    function stateText(f, progress) {
        switch (f.state) {
        case "downloading":
            if (f.asked) return qsTr("Asked again, waiting for an answer")
            if (refusedToUs(f)) return qsTr("Already sent to you once; ask to have it again")
            return f.size > 0 ? qsTr("Arriving… %1%").arg(Math.floor(100 * (f.received || 0) / f.size)) : qsTr("Arriving…")
        case "waiting": return f.reason ? qsTr("Waiting: %1").arg(f.reason) : qsTr("Not fetched yet")
        case "too_large": return f.max > 0 ? qsTr("Larger than your Puddle accepts (%1)").arg(root.chats.sizeText(f.max)) : qsTr("Too large for your Puddle")
        case "failed": return qsTr("Could not be fetched") + (f.reason ? ": " + f.reason : "")
        }
        return progress >= 0 ? qsTr("Opening… %1%").arg(Math.floor(100 * progress)) : ""
    }

    spacing: 6

    // ---- photos and videos ----
    Grid {
        visible: root.media.length > 0
        columns: root.media.length === 1 ? 1 : 2
        spacing: 4
        Repeater {
            model: root.media
            delegate: Rectangle {
                id: tile
                required property var modelData
                required property int index
                readonly property var f: modelData
                readonly property bool video: f.mime.startsWith("video/")
                readonly property bool here: f.state === "available"
                // one alone keeps its shape, at most 360 by 280; several are squares
                readonly property real aspect: f.w > 0 && f.h > 0 ? f.w / f.h : 4 / 3
                width: root.media.length === 1 ? Math.min(360, 280 * aspect) : 160
                height: root.media.length === 1 ? width / aspect : 160
                objectName: "mediaTile"
                radius: Theme.radiusLarge
                clip: true
                color: Theme.tint(palette.text, 0.08)
                Accessible.role: Accessible.Button
                Accessible.name: f.name

                property int rev: 0     // bumped when the file reaches the app's cache
                property real progress: -1
                Connections {
                    target: root.chats
                    function onFileReady(id) { if (id === tile.f.id) tile.rev++ }
                    function onFileProgress(id, p) { if (id === tile.f.id) tile.progress = p }
                }
                // a photo is brought into the app as soon as the Puddle has it; a video waits until it is played
                readonly property string local: rev >= 0 && here && !video ? root.chats.fileUrl(f.id, f.name) : ""

                Image { // the sender's preview, blurred, until the real thing is here
                    id: preview
                    anchors.fill: parent
                    visible: false
                    source: tile.f.preview ? "data:image/jpeg;base64," + tile.f.preview : ""
                    fillMode: Image.PreserveAspectCrop
                }
                MultiEffect {
                    anchors.fill: parent
                    source: preview
                    visible: tile.f.preview !== undefined && (tile.video || photo.status !== Image.Ready)
                    blurEnabled: true
                    blur: 1.0
                    blurMax: 32
                }
                Image {
                    id: photo
                    objectName: "mediaImage"
                    anchors.fill: parent
                    visible: !tile.video && status === Image.Ready
                    source: tile.local
                    sourceSize: Qt.size(720, 720)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    autoTransform: true
                }
                Rectangle { // play, and how long
                    visible: tile.video
                    anchors.centerIn: parent
                    width: 44; height: 44; radius: 22
                    color: Qt.rgba(0, 0, 0, 0.55)
                    Icon { anchors.centerIn: parent; glyph: "player-play"; size: 22; color: "white" }
                }
                Label {
                    visible: tile.video && tile.f.duration_ms > 0
                    anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6
                    text: root.clock(tile.f.duration_ms || 0)
                    color: "white"; font.pixelSize: 11
                    background: Rectangle { radius: 4; color: Qt.rgba(0, 0, 0, 0.55) }
                    leftPadding: 4; rightPadding: 4
                }
                // not here: what is happening, and a way to fetch it
                Column {
                    visible: !tile.here
                    anchors.centerIn: parent
                    width: parent.width - 16
                    spacing: 6
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: root.stateText(tile.f, -1)
                        color: "white"; font.pixelSize: 11
                        style: Text.Outline; styleColor: Qt.rgba(0, 0, 0, 0.5)
                    }
                    Button {
                        objectName: "downloadFile"
                        visible: (tile.f.state === "waiting" || tile.f.state === "failed") && !root.refusedToUs(tile.f)
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: tile.f.state === "failed" ? qsTr("Try again") : qsTr("Download %1").arg(root.chats.sizeText(tile.f.size))
                        onClicked: root.chats.fetchFile(root.messageId, tile.f.id)
                    }
                    Button {
                        objectName: "askAgain"
                        visible: root.refusedToUs(tile.f)
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Ask again")
                        onClicked: root.chats.requestFile(root.messageId, tile.f.id)
                    }
                }
                TapHandler {
                    enabled: tile.here
                    onTapped: root.viewMedia(root.media, tile.index)
                }
                HoverHandler { cursorShape: tile.here ? Qt.PointingHandCursor : Qt.ArrowCursor }
            }
        }
    }

    // ---- everything else ----
    Repeater {
        model: root.others
        delegate: Rectangle {
            id: card
            required property var modelData
            readonly property var f: modelData
            readonly property bool here: f.state === "available"
            objectName: "fileCard"
            Layout.preferredWidth: Math.min(360, root.width > 0 ? root.width : 360)
            implicitHeight: 56
            radius: Theme.radiusLarge
            color: Theme.tint(palette.text, 0.06)
            border.width: 1
            border.color: Theme.hairline
            property real progress: -1
            Connections {
                target: root.chats
                function onFileProgress(id, p) { if (id === card.f.id) card.progress = p }
                function onFileReady(id) { if (id === card.f.id) card.progress = -1 }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 10
                Icon { glyph: "file"; size: 26; opacity: 0.8 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label { objectName: "fileName"; Layout.fillWidth: true; text: card.f.name; elide: Text.ElideMiddle; font.bold: true }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: 11
                        opacity: 0.65
                        text: root.chats.sizeText(card.f.size) + (root.stateText(card.f, card.progress) ? "  ·  " + root.stateText(card.f, card.progress) : "")
                    }
                }
                Button {
                    objectName: "askAgain"
                    visible: root.refusedToUs(card.f)
                    text: qsTr("Ask again")
                    onClicked: root.chats.requestFile(root.messageId, card.f.id)
                }
                ToolButton {
                    objectName: "downloadFile"
                    visible: (card.f.state === "waiting" || card.f.state === "failed") && !root.refusedToUs(card.f)
                    icon.source: Theme.icon(card.f.state === "failed" ? "refresh" : "download"); icon.width: 18; icon.height: 18
                    display: AbstractButton.IconOnly
                    Accessible.name: card.f.state === "failed" ? qsTr("Try again") : qsTr("Download")
                    ToolTip.text: Accessible.name; ToolTip.visible: hovered
                    onClicked: root.chats.fetchFile(root.messageId, card.f.id)
                }
                ToolButton {
                    objectName: "saveFile"
                    visible: card.here
                    icon.source: Theme.icon("download"); icon.width: 18; icon.height: 18
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Save as…")
                    ToolTip.text: Accessible.name; ToolTip.visible: hovered
                    onClicked: root.saveFile(card.f)
                }
                Button {
                    objectName: "openFile"
                    visible: card.here
                    text: qsTr("Open")
                    onClicked: root.openFile(card.f)
                }
            }
        }
    }

    // ---- what passed between Puddles, as lines of their own ----
    Repeater {
        model: root.notes
        delegate: RowLayout {
            id: note
            required property var modelData
            readonly property var n: modelData
            objectName: n.kind === "refused" ? "fileRefused" : "fileRequest"
            Layout.fillWidth: true
            spacing: 8
            Icon { glyph: n.kind === "refused" ? "alert-circle" : "download"; size: 14; opacity: 0.6 }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
                opacity: 0.7
                text: note.n.kind === "refused"
                      ? qsTr("%1 accepts files up to %2, so %3 was not sent to them.").arg(root.chats.nameOf(note.n.user)).arg(root.chats.sizeText(note.n.max)).arg(note.n.f.name)
                      : qsTr("%1 asked for %2 again.").arg(root.chats.nameOf(note.n.user)).arg(note.n.f.name)
            }
            Button {
                objectName: "sendAgain"
                visible: note.n.kind === "request"
                text: qsTr("Send %1").arg(root.chats.sizeText(note.n.f.size))
                onClicked: root.chats.answerFileRequest(root.messageId, note.n.f.id, note.n.user, true)
            }
            Button {
                objectName: "declineAgain"
                visible: note.n.kind === "request"
                flat: true
                text: qsTr("Decline")
                onClicked: root.chats.answerFileRequest(root.messageId, note.n.f.id, note.n.user, false)
            }
        }
    }
}
