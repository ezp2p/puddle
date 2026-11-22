import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Server Settings, Applications (the Pond's docs/design.md, "Entry form"): those who answered the entry form and wait to be let
// in, with their answers; Accept makes them a member at once, Decline tells them (with a reason, if given) and they may apply
// again. Those already answered are under them.
ColumnLayout {
    id: pane
    required property var settings
    readonly property var ponds: settings.ponds
    spacing: 12

    Label {
        visible: pane.ponds.applications.count === 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.6
        text: qsTr("No applications. An entry form page that those joining must fill in brings them here.")
    }
    Repeater {
        model: pane.ponds.applications
        delegate: Rectangle {
            id: card
            required property var model
            readonly property bool waiting: model.status === "pending"
            objectName: "application_" + model.name
            Layout.fillWidth: true
            implicitHeight: col.implicitHeight + 24
            radius: Theme.radiusLarge
            color: waiting ? Theme.tint(Theme.primary, 0.08) : Theme.tint(Theme.text, Theme.hover)
            border.width: 1
            border.color: Theme.hairline
            opacity: waiting ? 1 : 0.7
            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: card.model.name; font.bold: true; font.pixelSize: 15 }
                    Label { text: new Date(card.model.created).toLocaleString(Qt.locale(), Locale.ShortFormat); opacity: 0.6; font.pixelSize: 12; Layout.fillWidth: true }
                    Label {
                        visible: !card.waiting
                        text: card.model.status === "accepted" ? qsTr("accepted") : qsTr("declined") + (card.model.reason ? ": " + card.model.reason : "")
                        opacity: 0.7
                    }
                }
                Repeater {
                    model: card.model.answers || []
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 1
                        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.label; opacity: 0.7; font.pixelSize: 12 }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: Array.isArray(modelData.answer) ? (modelData.answer.length ? modelData.answer.join(", ") : "—") : (modelData.answer || "—")
                        }
                    }
                }
                RowLayout {
                    visible: card.waiting
                    Layout.fillWidth: true
                    spacing: 8
                    TextField { id: why; Layout.fillWidth: true; placeholderText: qsTr("A reason, if you decline (they see it)"); maximumLength: 500 }
                    Button {
                        objectName: "declineApplication_" + card.model.name
                        text: qsTr("Decline")
                        palette.buttonText: Theme.danger
                        onClicked: pane.ponds.decideApplication(card.model.member, false, why.text)
                    }
                    PrimaryButton {
                        objectName: "acceptApplication_" + card.model.name
                        text: qsTr("Accept")
                        onClicked: pane.ponds.decideApplication(card.model.member, true, "")
                    }
                }
            }
        }
    }
}
