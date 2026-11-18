import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The first run (and whenever the owner asks again, from Settings): where is the owner's Puddle? It runs here, it runs somewhere
// else and this app connects to it as a phone would, or it ran somewhere that is gone and is rebuilt here from the recovery
// phrase. The answer is remembered (Backend::choose), so this is asked once.
Pane {
    id: welcome
    padding: 0

    required property var backend
    signal recover() // "Recover my Puddle": the setup screen, at its restore step

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

            Label {
                text: qsTr("Welcome to Puddle")
                font.pixelSize: 26
                font.bold: true
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Your Puddle keeps your identity, friends and chats on a computer you own: this one, or one that is "
                           + "always on, such as a home server. Where is yours?")
            }

            Repeater {
                model: [
                    { key: "here",
                      title: welcome.backend.hasLocalPuddle ? qsTr("Use the Puddle on this computer") : qsTr("Start a new Puddle here"),
                      text: welcome.backend.hasLocalPuddle
                            ? qsTr("Your Puddle is still on this computer, as you left it.")
                            : qsTr("I don't have one yet. It runs on this computer, so friends reach you while it's on.") },
                    { key: "remote",
                      title: qsTr("Connect to my Puddle"),
                      text: qsTr("It runs on another computer or a server. You'll need a pairing link from it.") },
                    { key: "recover",
                      title: qsTr("Recover my Puddle"),
                      text: qsTr("The computer it ran on is gone. Rebuild it here from your recovery phrase.") }
                ]
                delegate: Button {
                    id: choice
                    required property var modelData
                    objectName: "choose_" + modelData.key
                    visible: modelData.key !== "recover" || !welcome.backend.hasLocalPuddle
                    Layout.fillWidth: true
                    padding: 16
                    contentItem: ColumnLayout {
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: choice.modelData.title
                            font.bold: true
                            font.pixelSize: 16
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            text: choice.modelData.text
                        }
                    }
                    Accessible.name: modelData.title
                    onClicked: {
                        if (modelData.key === "recover") {
                            welcome.recover()
                            welcome.backend.choose("here")
                        } else {
                            welcome.backend.choose(modelData.key)
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.6
                font.pixelSize: 12
                text: qsTr("You can change this later in Settings.")
            }
        }
    }
}
