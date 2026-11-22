import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The "+" under the Ponds on the rail: paste the pond:// invite someone gave you, choose who the members will see you as, and join.
// A Pond with an entry form (the Pond's docs/design.md, "Entry form") asks its questions first: they are answered here, and the
// application waits for the Pond's admins; Check again (or joining with the same invite later) says where it stands.
Dialog {
    id: dlg

    required property var chats
    readonly property var ponds: chats.ponds

    // the entry form, while the Pond asks for one: {intro, questions}; and where the application stands
    property var form: null
    property string stage: "link"   // link, form, pending, declined
    property string reason: ""
    property var answers: ({})
    property string problem: ""

    function show() { link.text = ""; stage = "link"; form = null; answers = ({}); problem = ""; open(); link.forceActiveFocus() }
    function join() { if (link.text.trim() !== "" && !ponds.joining) { problem = ""; ponds.join(link.text, appearAs.profile) } }
    function apply() { problem = ""; ponds.join(link.text, appearAs.profile, answers) }
    function answer(id, value) { const a = Object.assign({}, answers); a[id] = value; answers = a }
    readonly property bool complete: form !== null && (form.questions || []).every(q => !q.required || (Array.isArray(answers[q.id]) ? answers[q.id].length > 0 : (answers[q.id] || "").trim() !== ""))

    objectName: "joinPondDialog"
    title: stage === "link" ? qsTr("Join a Pond") : stage === "form" ? qsTr("Apply to join") : qsTr("Your application")
    anchors.centerIn: Overlay.overlay
    modal: true
    width: Math.min(stage === "form" ? 560 : 440, Overlay.overlay ? Overlay.overlay.width - 24 : 440)
    footer: null

    Connections {
        target: dlg.ponds
        function onJoined() { dlg.close() }
        function onApplication(lnk, profile, code, pond) {
            if (code === "application_required" || (code === "application_declined" && dlg.stage !== "form")) {
                dlg.form = pond.form || { intro: "", questions: [] }
                dlg.reason = pond.reason || ""
                dlg.stage = code === "application_declined" ? "declined" : "form"
            } else if (code === "application_pending") {
                dlg.stage = "pending"
            } else {
                dlg.problem = pond.message || qsTr("The Pond didn't take that application.")
            }
        }
    }

    ColumnLayout {
        width: dlg.availableWidth
        spacing: 12

        // ---- the invite ----
        Label {
            visible: dlg.stage === "link"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.75
            text: qsTr("A Pond is a community server with channels. Paste the invite you were given.")
        }
        TextField {
            id: link
            objectName: "pondLink"
            visible: dlg.stage === "link"
            Layout.fillWidth: true
            placeholderText: "pond://pond.example.org?invite=…"
            onAccepted: dlg.join()
            FieldMenu {}
        }
        AppearAs { id: appearAs; visible: dlg.stage === "link"; chats: dlg.chats; Layout.fillWidth: true }

        // ---- the entry form ----
        ScrollView {
            id: formScroll
            visible: dlg.stage === "form"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(formCol.implicitHeight, Overlay.overlay ? Overlay.overlay.height - 260 : 420)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: formCol
                width: formScroll.availableWidth
                spacing: 12
                Text {
                    visible: dlg.form !== null && (dlg.form.intro || "") !== ""
                    Layout.fillWidth: true
                    textFormat: Text.MarkdownText
                    wrapMode: Text.Wrap
                    color: palette.text
                    text: dlg.form ? dlg.form.intro || "" : ""
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: qsTr("This Pond asks those joining a few questions. Its admins read your answers and let you in.")
                }
                Repeater {
                    model: dlg.stage === "form" && dlg.form ? dlg.form.questions : []
                    delegate: ColumnLayout {
                        id: q
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 4
                        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; font.bold: true; text: q.modelData.label + (q.modelData.required ? " *" : "") }
                        TextField {
                            visible: q.modelData.kind === "text"
                            objectName: "answer_" + q.modelData.id
                            Layout.fillWidth: true
                            maximumLength: 500
                            onTextChanged: if (visible) dlg.answer(q.modelData.id, text)
                        }
                        TextArea {
                            visible: q.modelData.kind === "long"
                            objectName: "answerLong_" + q.modelData.id
                            Layout.fillWidth: true
                            Layout.minimumHeight: 90
                            wrapMode: TextEdit.Wrap
                            onTextChanged: if (visible) dlg.answer(q.modelData.id, text)
                            background: Rectangle { radius: Theme.radius; color: Theme.tint(Theme.text, Theme.hover); border.width: 1; border.color: Theme.hairline }
                        }
                        Repeater {
                            model: q.modelData.kind === "choice" ? q.modelData.choices : []
                            delegate: RadioButton {
                                required property string modelData
                                text: modelData
                                onClicked: dlg.answer(q.modelData.id, modelData)
                            }
                        }
                        Repeater {
                            model: q.modelData.kind === "checks" ? q.modelData.choices : []
                            delegate: CheckBox {
                                required property string modelData
                                text: modelData
                                onToggled: {
                                    const now = (dlg.answers[q.modelData.id] || []).filter(c => c !== modelData)
                                    if (checked) now.push(modelData)
                                    dlg.answer(q.modelData.id, now)
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- where it stands ----
        Label {
            visible: dlg.stage === "pending"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Your application is waiting for the Pond's admins. Check again later, or join with the same invite: once you're accepted, you're in.")
        }
        Label {
            visible: dlg.stage === "declined"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dlg.reason !== "" ? qsTr("The Pond's admins declined your application: “%1”. You can answer again.").arg(dlg.reason)
                                    : qsTr("The Pond's admins declined your application. You can answer again.")
        }
        Label {
            visible: dlg.problem !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.danger
            text: dlg.problem
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: dlg.stage === "pending" ? qsTr("Close") : qsTr("Cancel"); onClicked: dlg.close() }
            PrimaryButton {
                objectName: "joinPondButton"
                visible: dlg.stage === "link"
                text: dlg.ponds.joining ? qsTr("Joining…") : qsTr("Join")
                enabled: link.text.trim() !== "" && !dlg.ponds.joining
                onClicked: dlg.join()
            }
            PrimaryButton {
                objectName: "applyButton"
                visible: dlg.stage === "form"
                text: dlg.ponds.joining ? qsTr("Sending…") : qsTr("Apply")
                enabled: dlg.complete && !dlg.ponds.joining
                onClicked: dlg.apply()
            }
            PrimaryButton {
                visible: dlg.stage === "declined"
                text: qsTr("Answer again")
                onClicked: { dlg.answers = ({}); dlg.stage = "form" }
            }
            PrimaryButton {
                objectName: "checkApplication"
                visible: dlg.stage === "pending"
                text: dlg.ponds.joining ? qsTr("Checking…") : qsTr("Check again")
                enabled: !dlg.ponds.joining
                onClicked: dlg.join()
            }
        }
    }
}
