import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../components"
import "../theme"

// A call's video (docs/calls.md, "Video"), under the call bar: a tile for each one whose camera is on (the friend, or each
// member of a group call), and this end's own, smaller and mirrored, while its camera is on. The pictures come from
// CallVideo, which decodes them off the window's thread and hands each to the sink of its tile.
Card {
    id: view

    required property var chats
    required property var video
    readonly property var calls: chats.calls
    readonly property var keys: video ? video.sources : []
    readonly property int tileWidth: keys.length <= 1 ? 480 : 320

    // who a tile is: a friend or member by id; on a phone, "" is the friend and "slot:<n>" a group call's member n
    function nameOf(key) {
        if (key === "")
            return calls.name
        if (key.startsWith("slot:"))
            return chats.nameOf(calls.slotUser(parseInt(key.substring(5))))
        return chats.nameOf(key)
    }

    objectName: "callVideo"
    visible: video !== null && video.running && (keys.length > 0 || video.cameraOn)
    implicitWidth: Math.min(content.implicitWidth, (parent ? parent.width : 1000) - 48) + 16
    implicitHeight: content.implicitHeight + 16

    Flow {
        id: content
        anchors.centerIn: parent
        width: Math.min(implicitWidth, view.implicitWidth - 16)
        spacing: 8

        Repeater {
            model: view.keys
            delegate: Rectangle {
                required property string modelData
                objectName: "videoTile"
                width: view.tileWidth
                height: width * 3 / 4
                radius: 8
                color: "black"
                clip: true
                VideoOutput {
                    id: out
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    Component.onCompleted: view.video.attach(parent.modelData, out.videoSink)
                }
                Label {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    text: view.nameOf(parent.modelData)
                    color: "white"
                    style: Text.Outline
                    styleColor: "black"
                }
            }
        }

        // this end, while its camera is on
        Rectangle {
            objectName: "selfTile"
            visible: view.video !== null && view.video.cameraOn
            width: 160
            height: 120
            radius: 8
            color: "black"
            clip: true
            VideoOutput {
                id: self
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectCrop
                transform: Scale { origin.x: self.width / 2; xScale: -1 } // as in a mirror
                Component.onCompleted: view.video.attach("self", self.videoSink)
            }
            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 6
                text: qsTr("You")
                color: "white"
                style: Text.Outline
                styleColor: "black"
            }
        }
    }
}
