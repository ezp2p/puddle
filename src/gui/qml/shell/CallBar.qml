import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The call going on (docs/calls.md), floating over the top of the page whatever is open: a friend ringing (answer or
// decline), ours ringing at theirs, connecting, or the call itself with how long it has run, mute and hang up. A friend
// rings on every device of the owner's; once one of them answers or starts a call, the others say where it is (on a phone,
// on the computer) and can only hang it up. A group's call shows the group, and how many others are in it; hanging up leaves
// it to them.
Card {
    id: bar

    required property var chats
    property var video: null        // the call's video (CallVideo); none, no camera button
    readonly property var calls: chats.calls
    readonly property string state: calls.state
    readonly property bool ringingHere: state === "ringing"
    readonly property bool inGroup: calls.group !== ""

    objectName: "callBar"
    visible: state !== "" || calls.error !== ""
    implicitWidth: row.implicitWidth + 24
    implicitHeight: row.implicitHeight + 16
    border.color: ringingHere ? Theme.primary : Theme.hairline
    border.width: ringingHere ? 2 : 1

    // the time since it connected, kept fresh while it runs
    property real now: Date.now()
    Timer { interval: 1000; repeat: true; running: bar.state === "active"; onTriggered: bar.now = Date.now() }

    function length() {
        const s = Math.max(0, Math.floor((bar.now - bar.calls.answeredAt) / 1000))
        const m = Math.floor(s / 60), h = Math.floor(m / 60)
        const two = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + two(m % 60) + ":" + two(s % 60) : m + ":" + two(s % 60)
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 10

        Avatar { visible: bar.state !== ""; chats: bar.chats; name: bar.calls.name; hash: ""; size: 28 }
        ColumnLayout {
            spacing: 0
            Label { visible: bar.state !== ""; text: bar.calls.name; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: 220 }
            Label {
                objectName: "callStatus"
                font.pixelSize: 12
                opacity: bar.state === "" ? 1 : 0.7
                color: bar.state === "" ? Theme.danger : palette.windowText
                readonly property string what: bar.state === "ringing" ? (bar.inGroup ? qsTr("%1 started a call").arg(bar.chats.nameOf(bar.calls.user)) : qsTr("is calling you"))
                    : bar.state === "calling" ? (bar.inGroup ? qsTr("Waiting for others…") : qsTr("Ringing…"))
                    : bar.state === "connecting" ? qsTr("Connecting…")
                    : bar.state === "active" ? (bar.inGroup ? bar.length() + " · " + (bar.calls.others === 1 ? qsTr("1 other") : qsTr("%1 others").arg(bar.calls.others)) : bar.length())
                    : bar.calls.error
                text: bar.calls.elsewhere !== "" ? what + " · " + bar.calls.elsewhere : what
            }
        }

        // a friend is ringing: answer or not
        PrimaryButton {
            objectName: "acceptCallButton"
            visible: bar.ringingHere
            text: bar.inGroup ? qsTr("Join") : qsTr("Answer")
            icon.source: Theme.icon("phone")
            onClicked: bar.calls.accept()
        }
        Button {
            objectName: "declineCallButton"
            visible: bar.ringingHere
            text: qsTr("Decline")
            palette.buttonText: Theme.danger
            onClicked: bar.calls.decline()
        }

        // in a call (or ringing theirs): mute, hang up
        ToolButton {
            objectName: "muteButton"
            visible: (bar.state === "active" || bar.state === "connecting") && bar.calls.here
            checkable: true
            checked: bar.calls.muted
            icon.source: Theme.icon(bar.calls.muted ? "microphone-off" : "microphone")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: bar.calls.muted ? qsTr("Unmute") : qsTr("Mute")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: bar.calls.toggleMute()
        }
        ToolButton {
            objectName: "cameraButton"
            visible: (bar.state === "active" || bar.state === "connecting") && bar.calls.here && bar.video !== null && bar.video.built
            checkable: true
            checked: bar.calls.camera
            icon.source: Theme.icon(bar.calls.camera ? "video" : "video-off")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: bar.calls.camera ? qsTr("Turn the camera off") : qsTr("Turn the camera on")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: bar.calls.toggleCamera()
        }
        // share the screen (in the window; a call a phone has carries none yet)
        ToolButton {
            objectName: "screenButton"
            visible: (bar.state === "active" || bar.state === "connecting") && bar.calls.here && bar.video !== null && bar.video.built && bar.video.canShareScreen
            checkable: true
            checked: bar.calls.screen
            icon.source: Theme.icon(bar.calls.screen ? "screen-share-off" : "screen-share")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            display: AbstractButton.IconOnly
            Accessible.name: bar.calls.screen ? qsTr("Stop sharing your screen") : qsTr("Share your screen")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: bar.calls.toggleScreen()
        }
        ToolButton {
            objectName: "hangUpButton"
            visible: bar.state !== "" && !bar.ringingHere
            icon.source: Theme.icon("phone-off")
            icon.width: Theme.iconSize
            icon.height: Theme.iconSize
            icon.color: Theme.danger
            display: AbstractButton.IconOnly
            Accessible.name: bar.inGroup ? qsTr("Leave the call") : bar.state === "calling" ? qsTr("Cancel the call") : qsTr("Hang up")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: bar.calls.hangUp()
        }
    }
}
