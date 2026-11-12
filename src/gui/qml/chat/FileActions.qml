import QtCore
import QtQuick
import QtQuick.Dialogs

// What the owner does with a file a message carries (docs/files.md): save it where they say, open it with the system's app,
// or look at a photo or video in the window (MediaViewer). Each waits for the file to reach the app's cache first. Shared by
// conversations and Pond channels.
Item {
    id: root

    required property var chats
    // the page's way of saying something went wrong
    signal failed(string why)

    property var pendingFile: null      // waiting to reach the app's cache, then `pendingAction` is done with it
    property string pendingAction: ""
    function whenHere(f, action) {
        if (chats.fileUrl(f.id, f.name) !== "") doFile(f, action)
        else { pendingFile = f; pendingAction = action }
    }
    function doFile(f, action) {
        if (action === "save") {
            saver.file = f
            saver.selectedFile = StandardPaths.writableUrl(StandardPaths.DownloadLocation) + "/" + encodeURIComponent(f.name)
            saver.open()
        } else if (!chats.openFile(f.id, f.name)) {
            root.failed(qsTr("%1 could not be opened.").arg(f.name))
        }
    }
    function save(f) { whenHere(f, "save") }
    function open(f) { whenHere(f, "open") }
    function view(list, i) { viewer.show(list, i) }
    Connections {
        target: root.chats
        function onFileReady(id) {
            if (root.pendingFile && id === root.pendingFile.id) {
                const f = root.pendingFile
                root.pendingFile = null
                root.doFile(f, root.pendingAction)
            }
        }
    }
    FileDialog {
        id: saver
        property var file: null
        title: qsTr("Save as")
        fileMode: FileDialog.SaveFile
        onAccepted: if (file) root.chats.saveFile(file.id, file.name, selectedFile)
    }
    MediaViewer {
        id: viewer
        chats: root.chats
        onSave: f => root.save(f)
    }
}
