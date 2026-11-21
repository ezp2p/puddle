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
    property real radius: width / 2   // round unless said (a Pond's icon on the rail is a rounded square)
    property string url: ""
    // a small picture in the corner: the Pond someone is met through (its icon's hash, and its name for the letter without one)
    property string badge: ""
    property string badgeName: ""
    readonly property bool badged: badge !== "" || badgeName !== ""
    readonly property bool ready: img.status === Image.Ready   // the picture shows, not the letter

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
        radius: av.radius
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
        sourceSize: Qt.size(av.width * 2, av.height * 2)
        asynchronous: true
    }
    Rectangle { id: mask; anchors.fill: parent; radius: av.radius; visible: false; layer.enabled: true }
    MultiEffect {
        anchors.fill: parent
        source: img
        maskEnabled: true
        maskSource: mask
        visible: img.status === Image.Ready
    }
    // the badge sits on a ring of the window's colour, so it reads apart from the picture under it
    Rectangle {
        visible: av.badged
        width: Math.max(14, Math.round(av.size * 0.45)) + 4
        height: width
        radius: width * 0.3
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: -3
        anchors.bottomMargin: -3
        color: av.palette.window
        Loader {
            anchors.centerIn: parent
            active: av.badged
            sourceComponent: Component {
                Item {
                    width: Math.max(14, Math.round(av.size * 0.45))
                    height: width
                    // an Avatar of its own, made only when wanted (an Avatar can't hold one directly)
                    Component.onCompleted: Qt.createComponent("Avatar.qml").createObject(this, {
                        chats: Qt.binding(() => av.chats), name: Qt.binding(() => av.badgeName), hash: Qt.binding(() => av.badge),
                        size: width, radius: width * 0.3,
                        objectName: "avatarBadge" })
                }
            }
        }
    }
}
