import QtQuick
import QtQuick.Controls

// The usual right-click menu of a text box: Undo, Redo, Cut, Copy, Paste and Select All. Put it inside the TextField or TextArea it is for.
ContextTrigger {
    id: root

    readonly property Item field: parent

    onRequested: (x, y) => {
        field.persistentSelection = true // the selection stays while the menu has the focus
        field.forceActiveFocus()
        menu.popup(field, x, y)
    }

    Menu {
        id: menu
        MenuItem { objectName: "menuUndo"; text: qsTr("Undo"); enabled: !root.field.readOnly && root.field.canUndo; onTriggered: root.field.undo() }
        MenuItem { objectName: "menuRedo"; text: qsTr("Redo"); enabled: !root.field.readOnly && root.field.canRedo; onTriggered: root.field.redo() }
        MenuSeparator {}
        MenuItem { objectName: "menuCut"; text: qsTr("Cut"); enabled: !root.field.readOnly && root.field.selectedText !== ""; onTriggered: root.field.cut() }
        MenuItem { objectName: "menuCopy"; text: qsTr("Copy"); enabled: root.field.selectedText !== ""; onTriggered: root.field.copy() }
        MenuItem { objectName: "menuPaste"; text: qsTr("Paste"); enabled: !root.field.readOnly && root.field.canPaste; onTriggered: root.field.paste() }
        MenuSeparator {}
        MenuItem { objectName: "menuSelectAll"; text: qsTr("Select All"); enabled: root.field.length > 0; onTriggered: root.field.selectAll() }
    }
}
