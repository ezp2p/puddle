import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../components"
import "../theme"

// A photo or a video of a message, over the whole window. A photo fits the window; the wheel or a double click zooms it,
// and dragging moves it about. A video plays from the copy in the app's cache (fetched from the Puddle first), with play
// and pause, seeking and the volume. Left and right go through the message's other photos and videos; Escape closes.
Popup {
    id: viewer

    required property var chats
    property var items: []
    property int index: 0
    readonly property var f: items.length > index ? items[index] : null
    readonly property bool video: f !== null && f.mime.startsWith("video/")
    signal save(var file)

    function show(list, i) { items = list; index = i; zoom = 1; open() }
    function step(d) { if (items.length > 1) { player.stop(); index = (index + d + items.length) % items.length; zoom = 1 } }

    property real zoom: 1
    property int rev: 0
    property real progress: -1
    Connections {
        target: viewer.chats
        function onFileReady(id) { if (viewer.f && id === viewer.f.id) { viewer.rev++; viewer.progress = -1 } }
        function onFileProgress(id, p) { if (viewer.f && id === viewer.f.id) viewer.progress = p }
    }
    readonly property string local: rev >= 0 && f !== null && visible ? chats.fileUrl(f.id, f.name) : ""

    objectName: "mediaViewer"
    parent: Overlay.overlay
    x: 0; y: 0
    width: parent ? parent.width : 800
    height: parent ? parent.height : 600
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    onClosed: player.stop()
    background: Rectangle { color: Qt.rgba(0, 0, 0, 0.92) }

    contentItem: Item {
        focus: true
        Keys.onLeftPressed: viewer.step(-1)
        Keys.onRightPressed: viewer.step(1)
        Keys.onSpacePressed: if (viewer.video) player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()

        // ---- a photo ----
        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 48
            visible: !viewer.video
            clip: true
            contentWidth: Math.max(width, width * viewer.zoom)
            contentHeight: Math.max(height, height * viewer.zoom)
            interactive: viewer.zoom > 1
            Image {
                id: photo
                objectName: "viewerImage"
                width: flick.contentWidth
                height: flick.contentHeight
                source: viewer.video ? "" : viewer.local
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                autoTransform: true
                TapHandler { onDoubleTapped: viewer.zoom = viewer.zoom > 1 ? 1 : 2.5 }
            }
            WheelHandler {
                acceptedModifiers: Qt.NoModifier
                onWheel: event => viewer.zoom = Math.max(1, Math.min(8, viewer.zoom * (event.angleDelta.y > 0 ? 1.15 : 1 / 1.15)))
            }
        }

        // ---- a video ----
        MediaPlayer {
            id: player
            source: viewer.video ? viewer.local : ""
            videoOutput: videoOut
            audioOutput: AudioOutput { id: audio; volume: volume.value }
            onSourceChanged: if (source.toString() !== "") play()
        }
        VideoOutput {
            id: videoOut
            objectName: "viewerVideo"
            anchors.fill: parent
            anchors.margins: 48
            anchors.bottomMargin: 96
            visible: viewer.video
            TapHandler { onTapped: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play() }
        }
        RowLayout {
            visible: viewer.video && viewer.local !== ""
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.margins: 32
            spacing: 12
            ToolButton {
                objectName: "playPause"
                icon.source: Theme.icon(player.playbackState === MediaPlayer.PlayingState ? "player-pause" : "player-play")
                icon.color: "white"; icon.width: 22; icon.height: 22
                display: AbstractButton.IconOnly
                Accessible.name: player.playbackState === MediaPlayer.PlayingState ? qsTr("Pause") : qsTr("Play")
                onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
            }
            Label { color: "white"; text: viewer.clock(player.position) }
            Slider {
                objectName: "seek"
                Layout.fillWidth: true
                from: 0; to: Math.max(1, player.duration)
                value: player.position
                enabled: player.seekable
                onMoved: player.position = value
                Accessible.name: qsTr("Position")
            }
            Label { color: "white"; text: viewer.clock(player.duration) }
            Slider { id: volume; Layout.preferredWidth: 90; from: 0; to: 1; value: 0.8; Accessible.name: qsTr("Volume") }
        }

        // ---- not here yet ----
        Label {
            anchors.centerIn: parent
            visible: viewer.local === "" && viewer.f !== null
            color: "white"
            font.pixelSize: 16
            text: viewer.progress >= 0 ? qsTr("Opening %1… %2%").arg(viewer.f ? viewer.f.name : "").arg(Math.floor(100 * viewer.progress))
                                       : qsTr("Opening %1…").arg(viewer.f ? viewer.f.name : "")
        }

        // ---- along the top: its name, where it is in the message, save, close ----
        RowLayout {
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.margins: 12
            spacing: 8
            Label {
                Layout.fillWidth: true
                color: "white"
                elide: Text.ElideMiddle
                text: viewer.f ? viewer.f.name + (viewer.items.length > 1 ? "   " + (viewer.index + 1) + " / " + viewer.items.length : "") : ""
            }
            ToolButton {
                objectName: "viewerSave"
                enabled: viewer.local !== ""
                icon.source: Theme.icon("download"); icon.color: "white"; icon.width: 20; icon.height: 20
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Save as…")
                ToolTip.text: Accessible.name; ToolTip.visible: hovered
                onClicked: viewer.save(viewer.f)
            }
            ToolButton {
                objectName: "viewerClose"
                icon.source: Theme.icon("x"); icon.color: "white"; icon.width: 20; icon.height: 20
                display: AbstractButton.IconOnly
                Accessible.name: qsTr("Close")
                onClicked: viewer.close()
            }
        }
        // the message's others
        ToolButton {
            visible: viewer.items.length > 1
            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 8
            icon.source: Theme.icon("chevron-left"); icon.color: "white"; icon.width: 28; icon.height: 28
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Previous")
            onClicked: viewer.step(-1)
        }
        ToolButton {
            visible: viewer.items.length > 1
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 8
            icon.source: Theme.icon("chevron-left"); icon.color: "white"; icon.width: 28; icon.height: 28
            rotation: 180
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Next")
            onClicked: viewer.step(1)
        }
    }

    function clock(ms) {
        const s = Math.floor(ms / 1000)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
}
