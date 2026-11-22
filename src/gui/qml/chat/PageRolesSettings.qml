import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A roles page's settings in its editor: which roles members may take for themselves, what each is for, and whether they may
// take several. A role can be offered only if it is below your own highest and gives nothing you can't do yourself.
ColumnLayout {
    id: s
    property var ponds
    property var data: ({})
    signal changed(var data)
    spacing: 8

    function offered(id) { return (data.roles || []).find(r => String(r.role) === id) }
    function set(id, on, description) {
        const roles = (data.roles || []).filter(r => String(r.role) !== id)
        if (on)
            roles.push({ role: Number(id), description: description || "" })
        const d = { roles: roles, multiple: data.multiple !== false }
        data = d
        changed(d)
    }

    Label { text: qsTr("Roles members may take"); font.bold: true }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.6
        text: qsTr("Give a channel to a role (its permissions, in the channel's settings) and offer the role here: members who take it see the channel. You can offer roles below your own that give nothing more than you can do.")
    }
    Repeater {
        model: s.ponds ? s.ponds.roles : null
        delegate: RowLayout {
            required property var model
            readonly property var o: s.offered(model.id)
            Layout.fillWidth: true
            spacing: 8
            CheckBox {
                objectName: "offerRole_" + model.name
                text: model.name
                checked: parent.o !== undefined
                enabled: model.editable === true || s.ponds.currentRole !== "member"
                onToggled: s.set(model.id, checked, desc.text)
            }
            TextField {
                id: desc
                Layout.fillWidth: true
                enabled: parent.o !== undefined
                placeholderText: qsTr("what it is for")
                maximumLength: 500
                text: parent.o ? parent.o.description : ""
                onEditingFinished: if (parent.o) s.set(model.id, true, text)
            }
        }
    }
    WrapCheck {
        objectName: "rolesMultiple"
        text: qsTr("Members may take several")
        checked: s.data.multiple !== false
        onToggled: { const d = { roles: s.data.roles || [], multiple: checked }; s.data = d; s.changed(d) }
    }
}
