import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The Pond voice channel you are in (docs/ponds.md, "Voice channels"), floating over the top of whatever page is open, as a
// call's bar does: which channel, in which Pond (a click takes you there), whether it is connected, and mute, deafen,
// camera, screen, the video in a window of its own, and leave (or, once it has ended by itself, dismiss).
Card {
    id: bar

    required property var chats
    property var video: null           // CallVideo: whether there is video at all
    property var view: null            // the shell's video pane state (popping it out)
    readonly property var ponds: chats.ponds
    readonly property string state: ponds.voiceState
    signal openRequested()             // the channel's name was clicked

    objectName: "voiceToast"
    visible: state !== ""
    implicitWidth: row.implicitWidth + 24
    implicitHeight: row.implicitHeight + 16
    border.color: state === "ended" ? Theme.danger : Theme.hairline

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8

        Icon { glyph: "volume"; size: 22; color: bar.state === "active" ? Theme.primary : bar.state === "ended" ? Theme.danger : palette.windowText }
        ColumnLayout {
            spacing: 0
            Label {
                objectName: "voiceToastName"
                text: bar.ponds.voiceName !== "" ? bar.ponds.voiceName : qsTr("Voice")
                font.bold: true
                font.underline: nameHover.hovered
                elide: Text.ElideRight
                Layout.maximumWidth: 220
                HoverHandler { id: nameHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: bar.openRequested() }
                Accessible.role: Accessible.Link
                Accessible.name: qsTr("Go to %1").arg(text)
            }
            Label {
                objectName: "voiceToastStatus"
                font.pixelSize: 12
                opacity: 0.7
                color: bar.state === "ended" ? Theme.danger : palette.windowText
                elide: Text.ElideRight
                Layout.maximumWidth: 260
                text: (bar.state === "active" ? qsTr("Voice connected") : bar.state === "connecting" ? qsTr("Connecting…")
                       : bar.ponds.voiceReason === "removed" ? qsTr("Disconnected by the Pond") : qsTr("Voice disconnected"))
                      + (bar.ponds.voicePondName !== "" ? " · " + bar.ponds.voicePondName : "")
            }
        }
        component BarButton: ToolButton {
            required property string glyph
            required property string tip
            icon.source: Theme.icon(glyph)
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: tip
            ToolTip.text: tip
            ToolTip.visible: hovered
        }
        BarButton {
            objectName: "voiceMute"
            visible: bar.state !== "ended"
            glyph: bar.ponds.voiceMuted ? "microphone-off" : "microphone"
            highlighted: bar.ponds.voiceMuted
            tip: bar.ponds.voiceMuted ? qsTr("Unmute") : qsTr("Mute")
            onClicked: bar.ponds.setVoiceMuted(!bar.ponds.voiceMuted)
        }
        BarButton {
            objectName: "voiceDeafen"
            visible: bar.state !== "ended"
            glyph: bar.ponds.voiceDeafened ? "headphones-off" : "headphones"
            highlighted: bar.ponds.voiceDeafened
            tip: bar.ponds.voiceDeafened ? qsTr("Undeafen") : qsTr("Deafen")
            onClicked: bar.ponds.setVoiceDeafened(!bar.ponds.voiceDeafened)
        }
        BarButton {
            objectName: "voiceCamera"
            visible: bar.state === "active" && bar.video !== null && bar.video.built
            glyph: bar.ponds.voiceCamera ? "video" : "video-off"
            highlighted: bar.ponds.voiceCamera
            tip: bar.ponds.voiceCamera ? qsTr("Turn the camera off") : qsTr("Turn the camera on")
            onClicked: bar.ponds.setVoiceCamera(!bar.ponds.voiceCamera)
        }
        BarButton {
            objectName: "voiceScreen"
            visible: bar.state === "active" && bar.video !== null && bar.video.built
            glyph: bar.ponds.voiceScreen ? "screen-share-off" : "screen-share"
            highlighted: bar.ponds.voiceScreen
            tip: bar.ponds.voiceScreen ? qsTr("Stop sharing your screen") : qsTr("Share your screen")
            onClicked: bar.ponds.setVoiceScreen(!bar.ponds.voiceScreen)
        }
        BarButton {
            objectName: "voicePopOut"
            visible: bar.state === "active" && bar.view !== null && bar.video !== null && bar.video.built
            glyph: "external-link"
            highlighted: bar.view !== null && bar.view.popped
            tip: bar.view !== null && bar.view.popped ? qsTr("Put the video back") : qsTr("Video in its own window")
            onClicked: { bar.view.popped = !bar.view.popped; bar.view.full = false }
        }
        BarButton {
            objectName: "voiceLeave"
            glyph: bar.state === "ended" ? "x" : "phone-off"
            icon.color: bar.state === "ended" ? palette.windowText : Theme.danger
            tip: bar.state === "ended" ? qsTr("Dismiss") : qsTr("Leave the voice channel")
            onClicked: bar.ponds.leaveVoice()
        }
    }
}
