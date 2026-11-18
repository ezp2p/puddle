import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"
import "../theme"

// First run, for a Puddle that runs here: make the identity and show its recovery phrase once. The Puddle does not start until
// the owner says the phrase is written down. Or (`recovering`) rebuild one from its phrase and a copy of its chain.
Pane {
    id: setup
    padding: 0

    required property var backend
    property bool recovering: false
    property url chainFile: ""
    property string recoverError: ""

    function back() {
        recovering = false
        recoverError = ""
        chainFile = ""
        backend.unchoose()
    }

    function restore() {
        recoverError = backend.restoreIdentity(recoverPhrase.text, chainFile)
        if (recoverError === "") {
            recoverPhrase.text = ""
            recovering = false
        }
    }

    // Held only while it is on screen, and cleared the moment setup is finished.
    property string phrase: ""
    readonly property var groups: phrase === "" ? [] : phrase.split("-")
    readonly property bool wide: width >= 560
    readonly property string mono: Qt.platform.os === "osx" ? "Menlo"
                                   : Qt.platform.os === "windows" ? "Consolas" : "monospace"

    function make() {
        const made = backend.createIdentity()
        if (made !== "")
            phrase = made
    }

    function finish() {
        phrase = ""
        written.checked = false
        backend.finishSetup()
    }

    ScrollArea {
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 48

        ColumnLayout {
            id: column
            width: Math.min(parent.width - 48, 560)
            x: (parent.width - width) / 2
            y: 24
            spacing: 16

            // ---- recovering: the phrase and a copy of the chain ----
            ColumnLayout {
                visible: setup.recovering && setup.phrase === ""
                Layout.fillWidth: true
                spacing: 16

                Label {
                    text: qsTr("Recover your Puddle")
                    font.pixelSize: 26
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("You need your latest recovery phrase (the one from your most recent key change) and a copy of "
                               + "your identity's chain: the file identity/chain.jsonl in your old Puddle's data folder, or in a "
                               + "backup of it. The chain isn't secret.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.banner
                    text: qsTr("Only do this if your old Puddle is gone. If it still runs, go back and choose \"Connect to my "
                               + "Puddle\": two Puddles with one identity get in each other's way.")
                }
                TextField {
                    id: recoverPhrase
                    objectName: "recoverPhrase"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Recovery phrase")
                    echoMode: TextInput.Password
                    font.family: setup.mono
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
                    Accessible.name: qsTr("Recovery phrase")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Button {
                        text: qsTr("Choose the chain file…")
                        onClicked: chainPicker.open()
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        opacity: 0.7
                        text: setup.chainFile.toString() === "" ? qsTr("No file chosen")
                                                                : decodeURIComponent(setup.chainFile.toString().replace(/^.*\//, ""))
                    }
                }
                Label {
                    visible: setup.recoverError !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.alert
                    text: setup.recoverError
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: qsTr("If the old computer might be in someone else's hands, change your keys once your Puddle is back, "
                               + "so whatever is left on it stops working.")
                }
                RowLayout {
                    spacing: 12
                    Button {
                        text: qsTr("Back")
                        onClicked: setup.back()
                    }
                    Button {
                        objectName: "recoverButton"
                        text: qsTr("Recover")
                        highlighted: true
                        enabled: recoverPhrase.text.trim() !== "" && setup.chainFile.toString() !== ""
                        onClicked: setup.restore()
                    }
                }
                FileDialog {
                    id: chainPicker
                    title: qsTr("Your identity's chain (chain.jsonl)")
                    fileMode: FileDialog.OpenFile
                    nameFilters: [qsTr("Chain (*.jsonl)"), qsTr("All files (*)")]
                    onAccepted: setup.chainFile = selectedFile
                }
            }

            // ---- step 1: before anything exists ----
            ColumnLayout {
                visible: setup.phrase === "" && !setup.recovering
                Layout.fillWidth: true
                spacing: 16

                Label {
                    text: "Welcome to Puddle"
                    font.pixelSize: 26
                    font.bold: true
                }
                Label {
                    visible: setup.backend.setupRedo
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.banner
                    text: "Setup wasn't finished last time, so it will start again. The identity from that attempt was never used."
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Puddle is going to make you an identity. It comes with a recovery phrase: eight groups of "
                          + "characters, shown once, on the next screen. It is the only way to get your identity back if "
                          + "this computer is lost, so have something to write it on, or a password manager, ready."
                }
                Label {
                    visible: setup.backend.error !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.alert
                    text: setup.backend.error
                }
                RowLayout {
                    spacing: 12
                    Button {
                        text: qsTr("Back")
                        onClicked: setup.back()
                    }
                    Button {
                        text: "Make my identity"
                        highlighted: true
                        onClicked: setup.make()
                    }
                }
            }

            // ---- step 2: the phrase ----
            ColumnLayout {
                visible: setup.phrase !== ""
                Layout.fillWidth: true
                spacing: 16

                Label {
                    text: "Your recovery phrase"
                    font.pixelSize: 26
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Write this down now. It is not saved anywhere on this computer and will not be shown again."
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: setup.wide ? 4 : 2
                    rowSpacing: 10
                    columnSpacing: 10

                    Repeater {
                        model: setup.groups
                        delegate: Rectangle {
                            id: card
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            implicitHeight: 62
                            radius: Theme.radiusLarge
                            color: "transparent"
                            border.width: 1
                            border.color: setup.palette.mid

                            Column {
                                anchors.centerIn: parent
                                spacing: 2
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: card.index + 1
                                    font.pixelSize: 11
                                    opacity: 0.55
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: card.modelData
                                    font.family: setup.mono
                                    font.pixelSize: 22
                                    font.bold: true
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Button {
                        text: "Copy"
                        onClicked: setup.backend.copyText(setup.phrase)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.7
                        text: "To keep it in a password manager, paste it there, then clear your clipboard."
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.banner
                    text: "Anyone who has this phrase can take over your identity. If you lose it and lose this "
                          + "computer, your identity is gone for good. When you change your keys later you will get a "
                          + "new phrase, and this one will stop working."
                }

                CheckBox {
                    id: written
                    text: "I have written my recovery phrase down"
                }
                Button {
                    text: "Continue"
                    enabled: written.checked
                    highlighted: true
                    onClicked: setup.finish()
                }
            }
        }
    }
}
