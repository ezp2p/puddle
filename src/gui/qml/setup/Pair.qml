import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The app on a phone, before it is let in: pair it with the owner's own Puddle (docs/identity.md, "Devices"). The owner
// makes a one-time link on the computer (Settings, "Add a phone"); this phone sends its name and a key of its own, and waits
// until the owner accepts it there, having checked the six digits are the same on both screens.
Pane {
    id: pair
    padding: 0

    required property var link   // the Phone object (backend.phoneLink)

    readonly property string mono: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"

    ScrollArea {
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 48

        ColumnLayout {
            id: column
            width: Math.min(parent.width - 32, 480)
            x: (parent.width - width) / 2
            y: 24
            spacing: 16

            Label {
                text: qsTr("Connect to your Puddle")
                font.pixelSize: 24
                font.bold: true
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }

            // ---- asking ----
            ColumnLayout {
                visible: pair.link.state === "unpaired" || pair.link.state === "pairing"
                Layout.fillWidth: true
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("This phone uses the Puddle on your computer. On the computer, open Puddle, go to Settings, choose "
                               + "\"Add a phone\", and copy the link it shows here.")
                }
                TextField {
                    id: linkField
                    objectName: "pairLink"
                    Layout.fillWidth: true
                    placeholderText: "puddle-pair://…"
                    font.family: pair.mono
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                    enabled: pair.link.state !== "pairing"
                    Accessible.name: qsTr("Pairing link")
                    FieldMenu {}
                }
                Label { text: qsTr("Call this phone"); opacity: 0.7 }
                TextField {
                    id: nameField
                    objectName: "pairName"
                    Layout.fillWidth: true
                    text: pair.link.defaultName()
                    maximumLength: 64
                    enabled: pair.link.state !== "pairing"
                    Accessible.name: qsTr("Name for this phone")
                    FieldMenu {}
                }
                Button {
                    objectName: "pairButton"
                    text: pair.link.state === "pairing" ? qsTr("Connecting…") : qsTr("Connect")
                    highlighted: true
                    enabled: pair.link.state !== "pairing" && linkField.text.trim() !== "" && nameField.text.trim() !== ""
                    onClicked: pair.link.pair(linkField.text, nameField.text)
                }
            }

            // ---- waiting for the owner ----
            ColumnLayout {
                visible: pair.link.state === "waiting"
                Layout.fillWidth: true
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Now accept this phone on your computer, in Puddle's Settings. Check it shows the same number:")
                }
                Label {
                    objectName: "pairCheck"
                    Layout.alignment: Qt.AlignHCenter
                    text: pair.link.check.length === 6 ? pair.link.check.slice(0, 3) + " " + pair.link.check.slice(3) : pair.link.check
                    font.family: pair.mono
                    font.pixelSize: 36
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: qsTr("If the numbers are different, don't accept it: something between this phone and your Puddle is not what it seems.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: qsTr("Waiting for %1…").arg(pair.link.address)
                }
                Button {
                    text: qsTr("Start over")
                    onClicked: pair.link.forget()
                }
            }

            Label {
                objectName: "pairError"
                visible: pair.link.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.alert
                text: pair.link.error
            }
        }
    }
}
