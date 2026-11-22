import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// An entry form's settings in its editor: its questions (a short or long answer, one choice or several), which need an answer,
// and whether those joining must fill it in. The page's markdown is the form's introduction.
ColumnLayout {
    id: s
    property var ponds
    property var data: ({})
    signal changed(var data)
    spacing: 8

    readonly property var questions: data.questions || []
    function put(qs, required) {
        const d = { questions: qs, required_to_join: required === undefined ? data.required_to_join === true : required }
        data = d
        changed(d)
    }
    function update(i, field, value) {
        const qs = JSON.parse(JSON.stringify(questions))
        qs[i][field] = value
        put(qs)
    }
    function nextId() {
        let n = questions.length + 1
        while (questions.some(q => q.id === "q" + n))
            n++
        return "q" + n
    }

    WrapCheck {
        objectName: "formRequired"
        text: qsTr("Those joining must fill this in, and wait to be accepted")
        checked: s.data.required_to_join === true
        onToggled: s.put(s.questions, checked)
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.6
        text: qsTr("The page's markdown is shown above the questions. Applications are answered in Server Settings, Applications.")
    }
    Repeater {
        model: s.questions
        delegate: Rectangle {
            required property var modelData
            required property int index
            Layout.fillWidth: true
            implicitHeight: qcol.implicitHeight + 16
            radius: Theme.radius
            color: Theme.tint(Theme.text, Theme.hover)
            ColumnLayout {
                id: qcol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        objectName: "questionLabel_" + index
                        Layout.fillWidth: true
                        placeholderText: qsTr("The question")
                        maximumLength: 500
                        text: modelData.label || ""
                        onEditingFinished: s.update(index, "label", text)
                    }
                    ComboBox {
                        model: [qsTr("Short answer"), qsTr("Long answer"), qsTr("One choice"), qsTr("Several choices")]
                        readonly property var ids: ["text", "long", "choice", "checks"]
                        currentIndex: Math.max(0, ids.indexOf(modelData.kind || "text"))
                        onActivated: i => s.update(index, "kind", ids[i])
                    }
                    CheckBox { text: qsTr("Needed"); checked: modelData.required === true; onToggled: s.update(index, "required", checked) }
                    ToolButton {
                        icon.source: Theme.icon("trash"); icon.width: 14; icon.height: 14; icon.color: Theme.danger
                        display: AbstractButton.IconOnly
                        Accessible.name: qsTr("Take this question off")
                        onClicked: { const qs = s.questions.slice(); qs.splice(index, 1); s.put(qs) }
                    }
                }
                TextField {
                    visible: modelData.kind === "choice" || modelData.kind === "checks"
                    Layout.fillWidth: true
                    placeholderText: qsTr("The choices, separated by commas")
                    text: (modelData.choices || []).join(", ")
                    onEditingFinished: s.update(index, "choices", text.split(",").map(c => c.trim()).filter(c => c !== ""))
                }
            }
        }
    }
    Button {
        objectName: "addQuestion"
        text: qsTr("Add a question")
        icon.source: Theme.icon("plus"); icon.width: 14; icon.height: 14
        enabled: s.questions.length < 30
        onClicked: { const qs = s.questions.slice(); qs.push({ id: s.nextId(), label: "", kind: "text", required: false, choices: [] }); s.put(qs) }
    }
}
