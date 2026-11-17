import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// A round picture, or the first letter of the name on a colour when there is none (or while it loads).
Item {
    id: av

    property var chats
    property string name: ""
    property string hash: ""
    property string tint: ""
    property int size: 40
    property string url: ""

    width: size
    height: size
    Accessible.role: Accessible.Graphic
    Accessible.name: name

    function load() {
        url = (chats && hash !== "") ? chats.avatarFor(hash) : ""
    }
    onHashChanged: load()
    Component.onCompleted: load()
    Connections {
        target: av.chats
        function onAvatarReady(h) { if (h === av.hash) av.load() }
    }

    Rectangle {
        id: disc
        anchors.fill: parent
        radius: width / 2
        // the same name always gets the same colour
        color: av.tint !== "" ? av.tint : Qt.hsla((av.name.length ? av.name.charCodeAt(0) * 37 % 360 : 200) / 360, 0.45, 0.42, 1)
        Label {
            anchors.centerIn: parent
            text: av.name.length ? av.name.charAt(0).toUpperCase() : "?"
            color: "white"
            font.bold: true
            font.pixelSize: av.size * 0.45
        }
    }
    Image {
        id: img
        anchors.fill: parent
        source: av.url
        visible: false
        fillMode: Image.PreserveAspectCrop
        sourceSize: Qt.size(av.size * 2, av.size * 2)
        asynchronous: true
    }
    Rectangle { id: mask; anchors.fill: parent; radius: width / 2; visible: false; layer.enabled: true }
    MultiEffect {
        anchors.fill: parent
        source: img
        maskEnabled: true
        maskSource: mask
        visible: img.status === Image.Ready
    }
}
