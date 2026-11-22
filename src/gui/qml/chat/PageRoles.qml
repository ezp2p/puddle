import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A roles page (the Pond's docs/design.md, "Pages"): the roles it offers, each with what it is for; a member takes and leaves
// them here, and sees the channels meant for them.
ColumnLayout {
    id: part
    property var chats
    property var page: ({})
    readonly property var ponds: chats ? chats.ponds : null
    readonly property var offered: (page.data && page.data.roles) || []
    readonly property bool multiple: !page.data || page.data.multiple !== false
    readonly property var mine: (page.mine || []).map(r => String(r))
    spacing: 10

    function roleOf(id) {
        if (!ponds)
            return null
        for (let i = 0; i < ponds.roles.count; i++) {
            const r = ponds.roles.get(i)
            if (r.id === id)
                return r
        }
        return null
    }
    function toggle(id, on) {
        let ids = multiple ? mine.filter(r => r !== id) : []
        if (on)
            ids.push(id)
        ponds.pickRoles(page.id, ids)
    }

    Label {
        visible: part.offered.length === 0
        text: qsTr("No roles to pick yet.")
        opacity: 0.6
    }
    Label {
        visible: part.offered.length > 0
        text: part.multiple ? qsTr("Pick any of these.") : qsTr("Pick one of these.")
        opacity: 0.7
    }
    Repeater {
        model: part.offered
        delegate: Rectangle {
            required property var modelData
            readonly property string rid: String(modelData.role)
            readonly property var role: part.roleOf(rid)
            readonly property bool held: part.mine.includes(rid)
            visible: role !== null
            Layout.fillWidth: true
            implicitHeight: row.implicitHeight + 20
            radius: Theme.radiusLarge
            color: held ? Theme.tint(Theme.primary, 0.12) : Theme.tint(Theme.text, Theme.hover)
            border.width: 1
            border.color: held ? Theme.tint(Theme.primary, 0.5) : Theme.hairline
            RowLayout {
                id: row
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12
                Rectangle { width: 14; height: 14; radius: 7; color: parent.parent.role && parent.parent.role.color ? parent.parent.role.color : Theme.tint(Theme.text, 0.3) }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: parent.parent.parent.role ? parent.parent.parent.role.name : ""; font.bold: true }
                    Label {
                        visible: text !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: parent.parent.parent.modelData.description || ""
                        opacity: 0.7
                        font.pixelSize: 13
                    }
                }
                Button {
                    objectName: "pickRole_" + (parent.parent.role ? parent.parent.role.name : "")
                    text: parent.parent.held ? qsTr("Leave") : qsTr("Take")
                    highlighted: !parent.parent.held
                    onClicked: part.toggle(parent.parent.rid, !parent.parent.held)
                }
            }
        }
    }
}
