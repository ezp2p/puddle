import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// An entry form page (the Pond's docs/design.md, "Entry form"): the questions those joining answer. Members see what is asked;
// whoever may manage applications sees how many wait, and goes to them in Server Settings.
ColumnLayout {
    id: part
    property var chats
    property var page: ({})
    readonly property var ponds: chats ? chats.ponds : null
    readonly property var questions: (page.data && page.data.questions) || []
    readonly property bool required: page.data && page.data.required_to_join === true
    signal openApplications()
    spacing: 10

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: note.implicitHeight + 20
        radius: Theme.radiusLarge
        color: part.required ? Theme.tint(Theme.primary, 0.12) : Theme.tint(Theme.text, Theme.hover)
        Label {
            id: note
            anchors.fill: parent
            anchors.margins: 10
            wrapMode: Text.Wrap
            text: part.required ? qsTr("Those joining answer these questions first, and get in once their application is accepted.")
                                : qsTr("This form is not asked of those joining yet: its editor says when it is.")
        }
    }
    RowLayout {
        visible: part.ponds && part.ponds.canManageApplications
        spacing: 8
        Label { text: qsTr("%n application(s) waiting", "", part.ponds ? part.ponds.applicationsWaiting : 0); font.bold: true }
        Button {
            objectName: "openApplications"
            text: qsTr("Open applications")
            onClicked: part.ponds.openSettings("applications")
        }
    }
    Repeater {
        model: part.questions
        delegate: ColumnLayout {
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: 4
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: (index + 1) + ". " + modelData.label + (modelData.required ? "  *" : "")
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.65
                font.pixelSize: 13
                text: modelData.kind === "text" ? qsTr("A short answer") : modelData.kind === "long" ? qsTr("A longer answer")
                    : modelData.kind === "choice" ? qsTr("One of: %1").arg(modelData.choices.join(", ")) : qsTr("Any of: %1").arg(modelData.choices.join(", "))
            }
        }
    }
}
