import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// First run: make the identity and show its recovery phrase once. The Puddle does not start until the
// owner says the phrase is written down.
Pane {
    id: setup
    padding: 0

    required property var backend

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

            // ---- step 1: before anything exists ----
            ColumnLayout {
                visible: setup.phrase === ""
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
                Button {
                    text: "Make my identity"
                    highlighted: true
                    onClicked: setup.make()
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
