import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// Someone's card, just under their name or picture where it was clicked, as Discord pops a profile out: a band in their
// colours, their picture, name and pronouns, their blurb, and in a Pond their role and whether they are muted. In a Puddle
// chat it shows what is known at once and fills in as their profile for the place on screen arrives (Chats.loadCard).
// showFor(item, card): `card` is Ponds.card(id) or Chats.card(id).
Popup {
    id: card

    objectName: "profileCard"
    property var chats
    property var info: ({})
    signal messageRequested(string userId)
    signal pondMessageRequested(string pond, string userId) // not a friend: a direct message through the Pond (docs/pond-dms.md)

    readonly property var colors: info.colors || []
    readonly property color nameColor: colors.length ? colors[0] : palette.text

    property Item anchorItem: null
    function showFor(item, data) {
        info = data
        anchorItem = item
        place()
        open()
        Qt.callLater(place) /* once it is laid out, its height is known */
        if (chats && data.role === undefined && !data.isMe)
            chats.loadCard(data.id)
    }
    // under the name, or over it when there is no room below
    function place() {
        if (!anchorItem || !parent)
            return
        const top = anchorItem.mapToItem(parent, 0, 0), h = Math.max(height, implicitHeight, 200)
        x = Math.max(8, Math.min(top.x, parent.width - width - 8))
        y = top.y + anchorItem.height + 4 + h + 8 <= parent.height ? top.y + anchorItem.height + 4 : Math.max(8, top.y - h - 4)
    }
    onHeightChanged: if (opened) place()
    function untilText(ms) {
        if (!(ms > 0))
            return qsTr("Muted until unmuted")
        const d = new Date(ms)
        return d.toDateString() === new Date().toDateString() ? qsTr("Muted until %1").arg(d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat))
                                                             : qsTr("Muted until %1").arg(d.toLocaleString(Qt.locale(), Locale.ShortFormat))
    }

    Connections {
        target: card.chats
        function onCardLoaded(id, c) {
            if (card.opened && id === card.info.id)
                card.info = Object.assign({}, card.info, c, { name: c.name || card.info.name, avatar: c.avatar || card.info.avatar })
        }
    }

    parent: Overlay.overlay
    width: 300
    padding: 0
    focus: true   // so Escape closes it
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }
    background: Rectangle { radius: Theme.radiusLarge + 4; color: Theme.card; border.width: 1; border.color: Theme.hairline }

    contentItem: ColumnLayout {
        spacing: 0

        // their colours: one, or the gradient they chose; a plain tint when they chose none
        Canvas {
            id: band
            objectName: "profileCardBand"
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            readonly property var stops: card.colors
            readonly property real angle: card.info.angle !== undefined ? card.info.angle : 180
            onStopsChanged: requestPaint()
            onAngleChanged: requestPaint()
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                const r = Theme.radiusLarge + 4
                ctx.reset()
                ctx.beginPath()
                ctx.moveTo(0, height); ctx.lineTo(0, r); ctx.arcTo(0, 0, r, 0, r)
                ctx.lineTo(width - r, 0); ctx.arcTo(width, 0, width, r, r); ctx.lineTo(width, height); ctx.closePath()
                if (stops.length > 1) {
                    // CSS-style angle: 180 runs top to bottom, 90 left to right
                    const a = (angle - 90) * Math.PI / 180, cx = width / 2, cy = height / 2
                    const half = Math.abs(width / 2 * Math.cos(a)) + Math.abs(height / 2 * Math.sin(a))
                    const g = ctx.createLinearGradient(cx - Math.cos(a) * half, cy - Math.sin(a) * half, cx + Math.cos(a) * half, cy + Math.sin(a) * half)
                    for (let i = 0; i < stops.length; i++)
                        g.addColorStop(i / (stops.length - 1), stops[i])
                    ctx.fillStyle = g
                } else {
                    ctx.fillStyle = stops.length ? stops[0] : Theme.tint(Theme.accent, 0.35)
                }
                ctx.fill()
            }
        }

        // the picture sits across the band's edge, ringed in the card's colour
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            Rectangle {
                x: 14; y: -38
                width: 80; height: 80; radius: 40
                color: Theme.card
                Avatar {
                    objectName: "profileCardAvatar"
                    anchors.centerIn: parent
                    chats: card.chats
                    name: card.info.name || ""
                    hash: card.info.avatar || ""
                    tint: card.colors.length ? card.colors[0] : ""
                    size: 72
                    // seen in a Pond, or met through one: its icon in the corner
                    badge: card.info.pond ? card.info.pondIcon || "" : ""
                    badgeName: card.info.pond ? card.info.pondName || card.info.place || "" : ""
                }
            }
            RoleBadge { anchors.right: parent.right; anchors.rightMargin: 14; anchors.top: parent.top; anchors.topMargin: 8; role: card.info.role || "" }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 14
            spacing: 4
            Label {
                objectName: "profileCardName"
                Layout.fillWidth: true
                text: card.info.name || ""
                font.bold: true
                font.pixelSize: 18
                color: card.nameColor
                elide: Text.ElideRight
            }
            Label {
                visible: text !== ""
                Layout.fillWidth: true
                text: card.info.pronouns || ""
                font.pixelSize: 12
                opacity: 0.7
            }
            RowLayout {
                visible: card.info.muted === true
                spacing: 6
                Icon { glyph: "microphone-off"; size: 14; color: Theme.danger }
                Label { objectName: "profileCardMuted"; text: card.untilText(card.info.mutedUntil); font.pixelSize: 12; color: Theme.danger }
            }
            Rectangle { visible: blurb.visible; Layout.fillWidth: true; Layout.topMargin: 6; Layout.bottomMargin: 4; height: 1; color: Theme.hairline }
            Label {
                id: blurb
                objectName: "profileCardBlurb"
                visible: text !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: card.info.blurb || ""
            }
            Label {
                visible: (card.info.place || "") !== ""
                Layout.fillWidth: true
                Layout.topMargin: 6
                text: qsTr("As they appear in %1").arg(card.info.place || "")
                font.pixelSize: 11
                opacity: 0.55
                elide: Text.ElideRight
            }
            Button {
                objectName: "profileCardMessage"
                visible: (card.info.friend === true || card.info.dms === true) && card.info.isMe !== true
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: qsTr("Message")
                icon.source: Theme.icon("message")
                onClicked: {
                    card.close()
                    if (card.info.friend === true)
                        card.messageRequested(card.info.id)
                    else
                        card.pondMessageRequested(card.info.pond, card.info.id)
                }
            }
        }
    }
}
