import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../theme"

// Everyone's video (docs/calls.md, "Video"; docs/ponds.md, "Voice channels"): a call's, or a Pond voice channel's. A grid of
// tiles, one for each camera and each shared screen coming in, and this end's own (its camera mirrored, its screen small),
// in the order they came: nobody moves when someone else comes or goes (or, if the owner chose it in Settings, those talking
// first). Clicking a tile pins it large, with the others in a strip under it; that is also how one of several shared screens
// is chosen to look at. The corner has full size (cover the messages) and pop out (a window of its own), or, in that window,
// put back. The pictures come from CallVideo, decoded off the window's thread.
Rectangle {
    id: pane

    required property var video        // CallVideo
    required property var view         // the shell's: full, popped, pinned (shared by the page and the window)
    property var backend: null          // who is talking, and the order chosen
    property var nameOf: key => key     // a sender's name, by the key their frames come with
    property bool inWindow: false       // this is the popped-out window's
    property var people: []             // who else is in it (ids, in the order they came): a tile each, video or not
    property var chats: null            // for pictures (Avatar)
    readonly property bool active: visible && video !== null && video.running

    // the tiles: each person in it (their camera, or their picture while it is off), then anyone else's video and every
    // screen shared, as they came, then this end (its camera or picture, and its screen)
    readonly property var keys: {
        if (!active)
            return []
        let all = people.slice()
        for (const k of video.sources)
            if (all.indexOf(k) < 0)
                all.push(k)
        if (backend && backend.videoOrder === "speaking") { /* those talking first, the rest as they came */
            const talking = k => backend.speaking.indexOf(k.endsWith("/screen") ? "" : k) >= 0
            all = all.filter(talking).concat(all.filter(k => !talking(k)))
        }
        all.push("self")
        if (video.screenOn)
            all.push("self/screen")
        return all
    }
    // whose video is coming in now (a tile without shows a picture)
    function hasVideo(key) { return key === "self" ? video.cameraOn : key === "self/screen" ? video.screenOn : video.sources.indexOf(key) >= 0 }
    readonly property string pinned: keys.indexOf(view.pinned) >= 0 ? view.pinned : ""

    function title(key) {
        if (key === "self")
            return qsTr("You")
        if (key === "self/screen")
            return qsTr("Your screen")
        if (key.endsWith("/screen"))
            return qsTr("%1's screen").arg(nameOf(key.substring(0, key.length - 7)))
        return nameOf(key)
    }
    function talking(key) {
        if (!backend || key.endsWith("/screen"))
            return false
        return backend.speaking.indexOf(key) >= 0
    }

    objectName: inWindow ? "videoWindowPane" : "videoPane"
    color: "black"
    radius: inWindow ? 0 : Theme.radiusLarge
    clip: true

    // one tile: a camera fills it (cropped), a screen fits (all of it shown)
    component Tile: Rectangle {
        id: tile
        required property string key
        property bool small: false
        readonly property bool screen: key.endsWith("/screen")
        readonly property bool live: pane.video !== null && pane.hasVideo(key)
        objectName: "videoTile_" + key
        color: "#111"
        radius: 6
        clip: true
        border.width: pane.talking(key) ? 3 : 0
        border.color: Theme.primary
        // no video from them: their picture
        Avatar {
            visible: !tile.live
            anchors.centerIn: parent
            chats: pane.chats
            name: tile.key === "self" && pane.chats ? pane.chats.myName : pane.title(tile.key)
            hash: tile.key === "self" && pane.chats ? pane.chats.myAvatar : ""
            size: Math.max(32, Math.min(96, Math.min(tile.width, tile.height) * 0.4))
        }
        VideoOutput {
            id: out
            visible: tile.live
            anchors.fill: parent
            anchors.margins: tile.border.width
            fillMode: tile.screen ? VideoOutput.PreserveAspectFit : VideoOutput.PreserveAspectCrop
            transform: Scale { origin.x: out.width / 2; xScale: tile.key === "self" ? -1 : 1 } // our own camera, as in a mirror
            Component.onCompleted: pane.video.attach(tile.key, out.videoSink)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 6
            radius: 4
            color: Qt.rgba(0, 0, 0, 0.55)
            implicitWidth: nameRow.implicitWidth + 10
            implicitHeight: nameRow.implicitHeight + 4
            RowLayout {
                id: nameRow
                anchors.centerIn: parent
                spacing: 4
                Icon { visible: tile.screen; glyph: "screen-share"; size: 13; color: "white" }
                Label { text: pane.title(tile.key); color: "white"; font.pixelSize: tile.small ? 11 : 13; elide: Text.ElideRight; Layout.maximumWidth: tile.width - 40 }
            }
        }
        Icon {
            visible: pane.pinned === tile.key || (tileHover.hovered && !tile.small)
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            glyph: "pin"
            size: 16
            color: "white"
            opacity: pane.pinned === tile.key ? 1 : 0.7
        }
        HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: pane.view.pinned = pane.pinned === tile.key ? "" : tile.key }
        Accessible.role: Accessible.Button
        Accessible.name: pane.pinned === tile.key ? qsTr("Unpin %1").arg(pane.title(tile.key)) : qsTr("Pin %1").arg(pane.title(tile.key))
        ToolTip.text: pane.pinned === tile.key ? qsTr("Click to see everyone") : qsTr("Click to pin")
        ToolTip.visible: tileHover.hovered
        ToolTip.delay: 800
    }

    // nobody pinned: a grid that fills the pane
    Item {
        id: gridArea
        visible: pane.pinned === ""
        anchors.fill: parent
        anchors.margins: 8
        readonly property int n: pane.pinned === "" ? pane.keys.length : 0
        readonly property int cols: n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4
        readonly property int rows: Math.max(1, Math.ceil(n / cols))
        readonly property real tileW: (width - (cols - 1) * 6) / cols
        readonly property real tileH: Math.min((height - (rows - 1) * 6) / rows, tileW * 9 / 16)
        Flow {
            anchors.centerIn: parent
            width: Math.min(parent.width, gridArea.cols * gridArea.tileW + (gridArea.cols - 1) * 6 + 1)
            spacing: 6
            Repeater {
                model: gridArea.n ? pane.keys : []
                delegate: Tile {
                    required property string modelData
                    key: modelData
                    width: gridArea.tileW
                    height: gridArea.tileH
                }
            }
        }
    }

    // one pinned: large, the others in a strip underneath
    ColumnLayout {
        visible: pane.pinned !== ""
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: pane.pinned !== ""
            sourceComponent: Tile { key: pane.pinned }
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 90
            visible: count > 0
            orientation: ListView.Horizontal
            spacing: 6
            clip: true
            model: pane.pinned !== "" ? pane.keys.filter(k => k !== pane.pinned) : []
            delegate: Tile {
                required property string modelData
                key: modelData
                small: true
                width: 160
                height: 90
            }
        }
    }


    // full size, pop out (or, in the window, put back): at the bottom, clear of a call's bar floating over the top of the page
    Row {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 8
        spacing: 4
        z: 2
        component PaneButton: ToolButton {
            required property string glyph
            required property string tip
            icon.source: Theme.icon(glyph)
            icon.color: "white"
            icon.width: 16
            icon.height: 16
            display: AbstractButton.IconOnly
            background: Rectangle { radius: 6; color: parent.hovered ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(0, 0, 0, 0.45) }
            Accessible.name: tip
            ToolTip.text: tip
            ToolTip.visible: hovered
        }
        PaneButton {
            objectName: "videoFull"
            visible: !pane.inWindow
            glyph: pane.view.full ? "arrows-minimize" : "arrows-maximize"
            tip: pane.view.full ? qsTr("Show the messages") : qsTr("Full size")
            onClicked: pane.view.full = !pane.view.full
        }
        PaneButton {
            objectName: "videoPopOut"
            glyph: pane.inWindow ? "arrows-minimize" : "external-link"
            tip: pane.inWindow ? qsTr("Put back") : qsTr("Pop out")
            onClicked: { pane.view.popped = !pane.inWindow; pane.view.full = false }
        }
    }
}
