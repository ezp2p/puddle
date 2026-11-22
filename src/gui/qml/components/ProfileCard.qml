import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// Someone's card, just under their name or picture where it was clicked, as Discord pops a profile out: a band in their
// colours (or their banner), their picture, name and pronouns, their blurb, and in a Pond their role and whether they are
// muted, and the roles they hold there (in each role's colour; someone who may give roles has a + to give or take them).
// Beside the picture, small icons: Message and Block, and in a Pond, for whoever may and is above them, Mute, Kick and Ban
// (asked through the PersonMenu's dialogs, so the card and the menu ask the same way). In a Puddle
// chat it shows what is known at once and fills in as their profile for the place on screen arrives (Chats.loadCard).
// showFor(item, card): `card` is Ponds.card(id) or Chats.card(id).
Popup {
    id: card

    objectName: "profileCard"
    // a small icon that does something about them, named in its tooltip
    component CardAction: ToolButton {
        property string glyph
        property string tip
        property bool danger: false
        implicitWidth: 32
        implicitHeight: 32
        padding: 6
        icon.source: Theme.icon(glyph)
        icon.width: 18
        icon.height: 18
        icon.color: danger ? Theme.danger : palette.buttonText
        display: AbstractButton.IconOnly
        Accessible.name: tip
        ToolTip.text: tip
        ToolTip.visible: hovered
        ToolTip.delay: 300
        background: Rectangle { radius: Theme.radius; color: parent.hovered ? Theme.tint(Theme.text, Theme.hover) : "transparent" }
    }
    property var chats
    property var ponds: null       // in a Pond: its operators' and administrators' buttons
    property var menu: null        // the PersonMenu, whose dialogs the buttons use (so they ask the same way)
    property var info: ({})
    // a change of roles, made from here or anywhere: the roles shown are the Pond's now
    Connections {
        target: card.ponds
        function onCurrentInfoChanged() {
            if (card.opened && card.info.pond && card.ponds && card.info.id)
                card.info = Object.assign({}, card.info, { roles: card.ponds.rolesOf(card.info.id) })
        }
    }
    property bool canMod: false    // we outrank them in this Pond (asked as the card opens: ranks change)
    property bool blocked: false
    signal messageRequested(string userId)
    signal pondMessageRequested(string pond, string userId) // not a friend: a direct message through the Pond (docs/pond-dms.md)

    readonly property var colors: info.colors || []
    readonly property color nameColor: colors.length ? colors[0] : palette.text

    property Item anchorItem: null
    function showFor(item, data) {
        info = data
        canMod = ponds !== null && !!data.id && data.isMe !== true && ponds.canActOn(data.id)
        blocked = !!chats && !!data.id && chats.isBlocked(data.id)
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
            // their banner, when they have one: over their colours, which show while it comes
            Avatar {
                id: banner
                objectName: "profileCardBanner"
                anchors.fill: parent
                chats: card.chats
                hash: card.info.banner || ""
                name: ""
                tint: "transparent"
                radius: Theme.radiusLarge + 4 /* the card's top corners */
                visible: hash !== "" && ready
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
            // what can be done about them, as small icons beside the picture: for anyone Message and Block; for an operator
            // or administrator who outranks them in a Pond, Mute, Kick and Ban
            RowLayout {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                visible: card.info.isMe !== true
                CardAction {
                    objectName: "profileCardMessage"
                    visible: card.info.friend === true || card.info.dms === true
                    glyph: "message"
                    tip: qsTr("Message")
                    onClicked: {
                        card.close()
                        if (card.info.friend === true)
                            card.messageRequested(card.info.id)
                        else
                            card.pondMessageRequested(card.info.pond, card.info.id)
                    }
                }
                CardAction {
                    objectName: "profileCardBlock"
                    visible: card.menu !== null
                    glyph: "ban"
                    tip: card.blocked ? qsTr("Unblock") : qsTr("Block")
                    danger: !card.blocked
                    onClicked: { card.close(); card.menu.askBlock(card.info) }
                }
                Rectangle { visible: card.canMod && card.menu !== null; width: 1; height: 18; color: Theme.hairline; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                CardAction {
                    objectName: card.info.muted === true ? "profileCardUnmute" : "profileCardMute"
                    visible: card.canMod && card.menu !== null && card.ponds !== null && card.ponds.canMute
                    glyph: card.info.muted === true ? "microphone" : "microphone-off"
                    tip: card.info.muted === true ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: { card.close(); card.info.muted === true ? card.menu.unmute(card.info) : card.menu.askMute(card.info) }
                }
                CardAction {
                    objectName: "profileCardKick"
                    visible: card.canMod && card.menu !== null && card.ponds.canKick
                    glyph: "door-exit"
                    tip: qsTr("Kick")
                    danger: true
                    onClicked: { card.close(); card.menu.askKick(card.info) }
                }
                CardAction {
                    objectName: "profileCardBan"
                    visible: card.canMod && card.menu !== null && card.ponds.canBan
                    glyph: "hammer"
                    tip: qsTr("Ban")
                    danger: true
                    onClicked: { card.close(); card.menu.askBan(card.info) }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 14
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "profileCardName"
                    Layout.fillWidth: true
                    text: card.info.name || ""
                    font.bold: true
                    font.pixelSize: 18
                    color: card.nameColor
                    elide: Text.ElideRight
                }
                RoleBadge { role: card.info.role || "" }
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
            // their roles in this Pond, highest first, as Discord's profile shows them
            ColumnLayout {
                id: rolesBox
                objectName: "profileCardRoles"
                readonly property var held: card.info.roles || []
                readonly property bool canGive: card.ponds !== null && card.info.pond !== undefined && card.ponds.canRoles &&
                                                card.ponds.canManageRoles && card.ponds.roles.count > 0
                visible: card.info.roles !== undefined && (held.length > 0 || canGive)
                Layout.fillWidth: true
                spacing: 4
                Rectangle { Layout.fillWidth: true; Layout.topMargin: 6; Layout.bottomMargin: 2; height: 1; color: Theme.hairline }
                Label { text: qsTr("Roles"); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: rolesBox.held
                        delegate: Rectangle {
                            required property var modelData
                            objectName: "profileCardRole_" + modelData.name
                            height: 22; radius: 6
                            width: chipRow.implicitWidth + 14
                            color: Theme.tint(Theme.text, Theme.hover)
                            border.width: 1
                            border.color: Theme.hairline
                            Row {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: 5
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 10; height: 10; radius: 5
                                    color: parent.parent.modelData.color || Theme.tint(Theme.text, 0.45)
                                }
                                Label { text: parent.parent.modelData.name; font.pixelSize: 12 }
                            }
                        }
                    }
                    // give or take a role (those below ours, unless we have a rank)
                    Rectangle {
                        objectName: "profileCardAddRole"
                        visible: rolesBox.canGive
                        height: 22; width: 22; radius: 6
                        color: plusHover.hovered ? Theme.tint(Theme.text, Theme.selected) : Theme.tint(Theme.text, Theme.hover)
                        Icon { anchors.centerIn: parent; glyph: "plus"; size: 14 }
                        HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                        // take the press for ourselves: the card is not modal, so a click it lets pass reaches whatever lies under it
                        TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: giveMenu.popup() }
                        ToolTip.text: qsTr("Roles"); ToolTip.visible: plusHover.hovered; ToolTip.delay: 300
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Give or take roles")
                        Menu {
                            id: giveMenu
                            Repeater {
                                model: card.ponds ? card.ponds.roles : null
                                delegate: MenuItem {
                                    required property var model
                                    objectName: "giveRole_" + model.name
                                    text: model.name
                                    checkable: true
                                    enabled: model.editable === true
                                    checked: rolesBox.held.some(r => r.id === model.id)
                                    onTriggered: card.ponds.setMemberRole(card.info.id, model.id, checked)
                                }
                            }
                        }
                    }
                }
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
        }
    }
}
