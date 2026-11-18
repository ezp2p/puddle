import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"
import "../theme"

// Settings, "Move to a server…" (docs/backup.md): the Puddle that runs here goes to a server, and this app uses it there. The owner
// says where it will be reached and where to save the file; Backend::handOver stops the Puddle here, adds this app to it as a
// device, writes it out, and puts the folder here aside. Then this says how to restore it on the server, and restarts the app.
Dialog {
    id: dlg

    required property var backend

    property int step: 0          // 0 asking, 1 writing, 2 done: what to do on the server
    property url file: ""
    property string problem: ""
    property string address: ""
    readonly property string fileName: decodeURIComponent(file.toString().replace(/^.*\//, ""))
    readonly property string mono: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"

    function show() { step = 0; problem = ""; file = ""; open(); addressField.forceActiveFocus() }

    objectName: "handOverDialog"
    title: step === 2 ? qsTr("Now, on the server") : qsTr("Move your Puddle to a server")
    anchors.centerIn: Overlay.overlay
    modal: true
    closePolicy: step === 0 ? Popup.CloseOnEscape : Popup.NoAutoClose
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 24 : 520)
    footer: null

    Connections {
        target: dlg.backend
        function onBackupFinished(error, file, handover) {
            if (!handover)
                return
            if (error !== "") {
                dlg.problem = error
                dlg.step = 0
            } else {
                dlg.step = 2
            }
        }
    }

    FileDialog {
        id: saver
        title: qsTr("Save your Puddle as")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "puddle"
        nameFilters: [qsTr("Puddle backup (*.puddle)")]
        onAccepted: {
            dlg.file = selectedFile
            dlg.address = addressField.text.trim()
            dlg.problem = ""
            dlg.step = 1
            if (!dlg.backend.handOver(dlg.address, dlg.file) && dlg.problem === "")
                dlg.step = 0
        }
    }

    ColumnLayout {
        width: dlg.availableWidth
        spacing: 12

        // ---- asking ----
        ColumnLayout {
            visible: dlg.step === 0
            Layout.fillWidth: true
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Your Puddle stops running on this computer, and runs on the server instead, with the same identity, "
                           + "friends, chats and phones. This app then uses it there.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("First, start Puddle on the server (the puddled image, with PUDDLE_ADVERTISE set), but don't set it "
                           + "up: no puddled --init. Then say where it is reached:")
            }
            TextField {
                id: addressField
                objectName: "handOverAddress"
                Layout.fillWidth: true
                placeholderText: "puddle.example.org:443"
                font.family: dlg.mono
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                Accessible.name: qsTr("The server's address")
                FieldMenu {}
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.banner
                text: qsTr("Nothing is deleted here: the folder is put aside, as %1.handed-over. But don't start it again "
                           + "once the server runs: two Puddles with one identity get in each other's way.").arg(dlg.backend.dataDir)
            }
            Label {
                visible: dlg.problem !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.alert
                text: dlg.problem
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 12
                Button { text: qsTr("Cancel"); onClicked: dlg.close() }
                Button {
                    text: qsTr("Save and move…")
                    highlighted: true
                    enabled: addressField.text.trim() !== ""
                    onClicked: saver.open()
                }
            }
        }

        // ---- writing ----
        RowLayout {
            visible: dlg.step === 1
            Layout.fillWidth: true
            spacing: 12
            BusyIndicator { running: dlg.step === 1 }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Writing your Puddle to %1…").arg(dlg.fileName) }
        }

        // ---- done: what to do there ----
        ColumnLayout {
            visible: dlg.step === 2
            Layout.fillWidth: true
            spacing: 10
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Your Puddle is in %1, and has stopped here. Copy that file to the server, then restore it there; it asks "
                           + "for your recovery phrase.").arg(dlg.fileName)
            }
            Label { text: qsTr("With Docker:"); font.bold: true }
            TextArea {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: dlg.mono
                font.pixelSize: 12
                text: "docker cp " + dlg.fileName + " puddle:/data/\n"
                      + "docker exec -it puddle puddled --restore --from /data/" + dlg.fileName
            }
            Label { text: qsTr("With Kubernetes:"); font.bold: true }
            TextArea {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: dlg.mono
                font.pixelSize: 12
                text: "kubectl cp " + dlg.fileName + " <pod>:/data/\n"
                      + "kubectl exec -it <pod> -- puddled --restore --from /data/" + dlg.fileName
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("A Puddle waiting to be set up starts by itself once it is restored. Delete the file from the server "
                           + "afterwards; keep a copy somewhere safe as a backup if you like (only your phrase opens it).")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Then restart Puddle here: it connects to your Puddle at %1.").arg(dlg.address)
            }
            Button {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Restart Puddle")
                highlighted: true
                onClicked: dlg.backend.restartApp()
            }
        }
    }
}
