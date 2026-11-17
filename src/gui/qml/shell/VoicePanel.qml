import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The voice you are in, at the foot of the left-hand list just above your own row, as Discord has it: a Pond's voice
// channel (docs/ponds.md, "Voice channels") or a call (docs/calls.md), the same way for both. It slides up when one starts
// (or rings) and away when it is over. The top row is where (the channel or the conversation; a click takes you there) and
// how it is going, the row under it what can be done: answer or decline a friend ringing; mute, deafen (a voice channel),
// camera, screen, the video in a window of its own, leave or hang up; dismiss what went wrong. A call answered on another
// of the owner's devices says where, and can only be hung up.
Item {
    id: panel

    required property var chats
    property var video: null           // CallVideo: whether there is video at all
    property var view: null            // the shell's video pane state (popping it out)
    signal openRequested()             // the name was clicked

    readonly property var ponds: chats.ponds
    readonly property var calls: chats.calls
    readonly property bool inVoice: ponds.voiceState !== ""
    readonly property bool inCall: !inVoice && (calls.state !== "" || calls.error !== "")
    readonly property bool shown: inVoice || inCall
    readonly property bool hasVideo: video !== null && video.built
    readonly property string phase: inVoice ? ponds.voiceState : calls.state
    readonly property bool ringing: inCall && phase === "ringing"
    readonly property bool failed: inCall && phase === ""   // only an error to show
    // sound and pictures can be changed: the voice channel joined, or a call going on here
    readonly property bool live: inVoice ? phase !== "ended" : calls.here && (phase === "active" || phase === "connecting")
    readonly property bool flowing: inVoice ? phase === "active" : live

    objectName: "voicePanel"
    clip: true
    implicitHeight: shown ? box.implicitHeight : 0
    Behavior on implicitHeight { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    visible: implicitHeight > 0

    // the time since the call connected
    property real now: Date.now()
    Timer { interval: 1000; repeat: true; running: panel.inCall && panel.phase === "active"; onTriggered: panel.now = Date.now() }
    function length() {
        const s = Math.max(0, Math.floor((panel.now - panel.calls.answeredAt) / 1000))
        const m = Math.floor(s / 60), h = Math.floor(m / 60)
        const two = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + two(m % 60) + ":" + two(s % 60) : m + ":" + two(s % 60)
    }
    function status() {
        if (inVoice)
            return (phase === "active" ? qsTr("Voice connected") : phase === "connecting" ? qsTr("Connecting…")
                    : ponds.voiceReason === "removed" ? qsTr("Disconnected by the Pond") : qsTr("Voice disconnected"))
                   + (ponds.voicePondName !== "" ? " · " + ponds.voicePondName : "")
        const group = calls.group !== ""
        const what = phase === "ringing" ? (group ? qsTr("%1 started a call").arg(chats.nameOf(calls.user)) : qsTr("is calling you"))
            : phase === "calling" ? (group ? qsTr("Waiting for others…") : qsTr("Ringing…"))
            : phase === "connecting" ? qsTr("Connecting…")
            : phase === "active" ? (group ? length() + " · " + (calls.others === 1 ? qsTr("1 other") : qsTr("%1 others").arg(calls.others))
                                          : qsTr("In a call") + " · " + length())
            : calls.error
        return calls.elsewhere !== "" ? what + " · " + calls.elsewhere : what
    }

    Rectangle {
        id: box
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom   // slides up from the row beneath
        implicitHeight: col.implicitHeight + 16
        color: panel.ringing ? Qt.alpha(Theme.primary, 0.12) : "transparent"
        Rectangle { width: parent.width; height: 1; color: Theme.hairline }

        ColumnLayout {
            id: col
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 4

            RowLayout {
                spacing: 8
                Layout.fillWidth: true
                Icon {
                    glyph: panel.inVoice ? "volume" : "phone"
                    size: 20
                    color: panel.phase === "active" || panel.ringing ? Theme.primary
                         : panel.phase === "ended" || panel.failed ? Theme.danger : palette.windowText
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Label {
                        objectName: "voicePanelName"
                        Layout.fillWidth: true
                        text: panel.inVoice ? (panel.ponds.voiceName !== "" ? panel.ponds.voiceName : qsTr("Voice"))
                              : panel.failed ? qsTr("Call") : panel.calls.name
                        font.bold: true
                        font.underline: nameHover.hovered && !panel.failed
                        elide: Text.ElideRight
                        HoverHandler { id: nameHover; enabled: !panel.failed; cursorShape: Qt.PointingHandCursor }
                        TapHandler { enabled: !panel.failed; onTapped: panel.openRequested() }
                        Accessible.role: Accessible.Link
                        Accessible.name: qsTr("Go to %1").arg(text)
                    }
                    Label {
                        objectName: "voicePanelStatus"
                        Layout.fillWidth: true
                        font.pixelSize: 11
                        opacity: panel.failed ? 1 : 0.7
                        wrapMode: panel.failed ? Text.Wrap : Text.NoWrap
                        elide: Text.ElideRight
                        color: panel.phase === "ended" || panel.failed ? Theme.danger : palette.windowText
                        text: panel.status()
                    }
                }
            }

            // a friend ringing: answer or not
            RowLayout {
                visible: panel.ringing
                spacing: 8
                Layout.fillWidth: true
                PrimaryButton {
                    objectName: "voicePanelAnswer"
                    Layout.fillWidth: true
                    text: panel.calls.group !== "" ? qsTr("Join") : qsTr("Answer")
                    icon.source: Theme.icon("phone")
                    onClicked: panel.calls.accept()
                }
                Button {
                    objectName: "voicePanelDecline"
                    Layout.fillWidth: true
                    text: qsTr("Decline")
                    palette.buttonText: Theme.danger
                    onClicked: panel.calls.decline()
                }
            }

            component PanelButton: ToolButton {
                required property string glyph
                required property string tip
                property bool lit: false
                icon.source: Theme.icon(glyph)
                icon.width: Theme.iconSize
                icon.height: Theme.iconSize
                display: AbstractButton.IconOnly
                highlighted: lit
                Accessible.name: tip
                ToolTip.text: tip
                ToolTip.visible: hovered
                Layout.fillWidth: true
            }
            RowLayout {
                visible: !panel.ringing
                spacing: 0
                Layout.fillWidth: true
                PanelButton {
                    objectName: "voicePanelMute"
                    visible: panel.live
                    readonly property bool muted: panel.inVoice ? panel.ponds.voiceMuted : panel.calls.muted
                    lit: muted
                    glyph: muted ? "microphone-off" : "microphone"
                    tip: muted ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: panel.inVoice ? panel.ponds.setVoiceMuted(!muted) : panel.calls.toggleMute()
                }
                PanelButton {
                    objectName: "voicePanelDeafen"
                    visible: panel.inVoice && panel.live
                    lit: panel.ponds.voiceDeafened
                    glyph: lit ? "headphones-off" : "headphones"
                    tip: lit ? qsTr("Undeafen") : qsTr("Deafen")
                    onClicked: panel.ponds.setVoiceDeafened(!panel.ponds.voiceDeafened)
                }
                PanelButton {
                    objectName: "voicePanelCamera"
                    visible: panel.hasVideo && panel.flowing
                    lit: panel.inVoice ? panel.ponds.voiceCamera : panel.calls.camera
                    glyph: lit ? "video" : "video-off"
                    tip: lit ? qsTr("Turn the camera off") : qsTr("Turn the camera on")
                    onClicked: panel.inVoice ? panel.ponds.setVoiceCamera(!lit) : panel.calls.toggleCamera()
                }
                PanelButton {
                    objectName: "voicePanelScreen"
                    visible: panel.hasVideo && panel.video.canShareScreen && panel.flowing
                    lit: panel.inVoice ? panel.ponds.voiceScreen : panel.calls.screen
                    glyph: lit ? "screen-share-off" : "screen-share"
                    tip: lit ? qsTr("Stop sharing your screen") : qsTr("Share your screen")
                    onClicked: panel.inVoice ? panel.ponds.setVoiceScreen(!lit) : panel.calls.toggleScreen()
                }
                PanelButton {
                    objectName: "voicePanelPopOut"
                    visible: panel.hasVideo && panel.view !== null && panel.phase === "active" && (panel.inVoice || panel.calls.here)
                    lit: panel.view !== null && panel.view.popped
                    glyph: "external-link"
                    tip: lit ? qsTr("Put the video back") : qsTr("Video in its own window")
                    onClicked: { panel.view.popped = !panel.view.popped; panel.view.full = false }
                }
                PanelButton {
                    objectName: "voicePanelLeave"
                    readonly property bool over: panel.failed || (panel.inVoice && panel.phase === "ended")
                    glyph: over ? "x" : "phone-off"
                    icon.color: over ? palette.windowText : Theme.danger
                    tip: over ? qsTr("Dismiss") : panel.inVoice ? qsTr("Leave the voice channel")
                         : panel.calls.group !== "" ? qsTr("Leave the call") : panel.phase === "calling" ? qsTr("Cancel the call") : qsTr("Hang up")
                    onClicked: panel.inVoice ? panel.ponds.leaveVoice() : panel.failed ? panel.calls.dismissError() : panel.calls.hangUp()
                }
            }
        }
    }
}
