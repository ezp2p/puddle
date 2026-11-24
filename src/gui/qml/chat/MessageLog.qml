import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The open channel's message log (docs/ponds.md, "Editing and deleting"), for whoever may read it there: each message that was
// edited or deleted, newest first, with every text it had and its files (to save, as evidence if need be), and, for whoever
// may also manage messages, Erase for good on a deleted one. show("") the whole log; show(id) one message's history.
Dialog {
    id: dlg
    objectName: "messageLog"

    required property var ponds
    required property var chats
    property string only: ""           // one message's history, or "" for the channel's log
    signal saveFile(var file)
    signal openFile(var file)

    function show(messageId) {
        only = messageId
        if (messageId !== "") ponds.loadVersions(messageId)
        else ponds.loadLog()
        open()
    }

    anchors.centerIn: Overlay.overlay
    modal: true
    width: Math.min(640, Overlay.overlay ? Overlay.overlay.width - 48 : 640)
    height: Math.min(720, Overlay.overlay ? Overlay.overlay.height - 48 : 720)
    title: only !== "" ? qsTr("Edit history") : qsTr("Message log — #%1").arg(ponds.channelName)
    standardButtons: Dialog.Close

    function when(ts) { return new Date(ts).toLocaleString(Qt.locale(), Locale.ShortFormat) }

    // a file of the log is fetched through the message it was in, as the Pond keeps it for the log
    QtObject {
        id: logFetcher
        function fetchFile(messageId, fileId) { dlg.ponds.fetchLogFile(messageId, fileId) }
    }
    ConfirmDialog { id: confirm }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.65
            visible: dlg.only === ""
            text: qsTr("Messages here that were edited or deleted. Only those who may read the message log see this.")
        }
        Label {
            visible: list.count === 0
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.55
            text: qsTr("Nothing has been edited or deleted here.")
        }

        ScrollList {
            id: list
            objectName: "messageLogList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: dlg.ponds.log
            spacing: 12
            onUserScrolled: if (contentY + height >= contentHeight - 40 && dlg.ponds.logHasOlder) dlg.ponds.loadLog(true)

            delegate: Rectangle {
                id: entry
                required property var model
                width: ListView.view.width
                implicitHeight: body.implicitHeight + 20
                radius: Theme.radius
                color: Theme.card
                border.color: entry.model.deleted ? Theme.tint(Theme.danger, 0.5) : Theme.hairline

                ColumnLayout {
                    id: body
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: entry.model.name; font.bold: true }
                        Label { text: dlg.when(entry.model.ts); font.pixelSize: 11; opacity: 0.55; Layout.fillWidth: true }
                        Button {
                            objectName: "messageLogPurge"
                            visible: entry.model.deleted && dlg.ponds.canPurgeHere
                            text: qsTr("Erase for good…")
                            palette.buttonText: Theme.danger
                            onClicked: confirm.ask(qsTr("Erase for good"),
                                                   qsTr("Erase this message, every text it had, and its files from the Pond? Nobody can bring them back. Save any file you need first."),
                                                   qsTr("Erase"), () => dlg.ponds.purgeMessage(entry.model.id))
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: entry.model.deleted ? Theme.danger : palette.text
                        opacity: entry.model.deleted ? 1 : 0.65
                        text: entry.model.deleted ? qsTr("Deleted by %1, %2").arg(entry.model.deletedByName).arg(dlg.when(entry.model.deletedTs))
                                                  : qsTr("Edited %1").arg(dlg.when(entry.model.edited))
                    }

                    // every earlier text, oldest first, then the last
                    Repeater {
                        model: entry.model.versions || []
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: qsTr("Until %1").arg(dlg.when(modelData.until)); font.pixelSize: 11; opacity: 0.55 }
                            TextEdit {
                                Layout.fillWidth: true
                                readOnly: true; selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                color: palette.text; opacity: 0.8
                                textFormat: TextEdit.PlainText
                                text: modelData.text !== "" ? modelData.text : qsTr("(no words)")
                                font.italic: modelData.text === ""
                            }
                            Label {
                                visible: modelData.files.length > 0
                                font.pixelSize: 11; opacity: 0.55
                                text: qsTr("with %n file(s)", "", modelData.files.length)
                            }
                        }
                    }
                    Label {
                        visible: (entry.model.versions || []).length > 0
                        text: entry.model.deleted ? qsTr("Last, when deleted") : qsTr("Now")
                        font.pixelSize: 11; opacity: 0.55
                    }
                    TextEdit {
                        objectName: "messageLogText"
                        Layout.fillWidth: true
                        readOnly: true; selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: palette.text
                        textFormat: TextEdit.PlainText
                        text: entry.model.text !== "" ? entry.model.text : qsTr("(no words)")
                        font.italic: entry.model.text === ""
                    }
                    MessageFiles {
                        visible: !!entry.model.files && entry.model.files.length > 0
                        Layout.fillWidth: true
                        chats: dlg.chats
                        fetcher: logFetcher
                        files: entry.model.files || []
                        messageId: entry.model.id
                        onSaveFile: f => dlg.saveFile(f)
                        onOpenFile: f => dlg.openFile(f)
                        onViewMedia: (l, i) => dlg.openFile(l[i])
                    }
                    Label {
                        visible: (entry.model.removed || []).length > 0
                        font.pixelSize: 11; opacity: 0.55
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("%n of these file(s) an edit took off the message.", "", (entry.model.removed || []).length)
                    }
                }
            }
        }
    }
}
