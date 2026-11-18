import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"
import "../theme"

// Make or change one of the owner's profiles (docs/profiles.md): how they appear to the people and groups they choose. The Puddle
// keeps it, wherever it runs, so this works the same in the app the Puddle runs in and on a phone or computer connected to it;
// pictures are sent to the Puddle as they are chosen (Chats::uploadPicture), and the profile names them by hash.
Dialog {
    id: dlg

    required property var chats

    property string profileId: ""   // "" while making a new one
    property bool isNew: profileId === ""
    property var original: ({})
    // pictures by slot: the hash shown now ("" for none), and which are on their way to the Puddle
    property var pictures: ({ avatar: "", banner: "", background: "" })
    property var uploading: ({})
    property bool colorIsGradient: false
    property bool saving: false
    property string problem: ""
    readonly property bool busy: saving || Object.keys(uploading).length > 0
    readonly property string mono: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"

    function hashOf(v) { return typeof v === "string" ? v : (v && v.hash) ? v.hash : "" }
    function textOf(v) { return typeof v === "string" ? v : "" }

    // open it for `id`, or for a new profile with no id
    function edit(id) {
        profileId = id || ""
        original = isNew ? ({}) : chats.profile(profileId)
        nameField.text = textOf(original.display_name)
        pronounsField.text = textOf(original.pronouns)
        blurbField.text = textOf(original.blurb)
        aboutField.text = textOf(original.about)
        colorIsGradient = original.color !== undefined && original.color !== null && typeof original.color === "object"
        colorField.text = colorIsGradient ? "" : textOf(original.color)
        pictures = { avatar: hashOf(original.avatar), banner: hashOf(original.banner), background: hashOf(original.background) }
        uploading = ({})
        links.clear()
        for (const l of (original.links || []))
            links.append({ text: l.text || "", url: l.url || "" })
        problem = ""
        saving = false
        open()
        nameField.forceActiveFocus()
    }

    function setPicture(slot, hash) {
        const p = Object.assign({}, pictures)
        p[slot] = hash
        pictures = p
    }

    function save() {
        const name = nameField.text.trim()
        if (name === "") {
            problem = qsTr("A profile needs a name.")
            return
        }
        const color = colorField.text.trim()
        if (color !== "" && !/^#[0-9a-fA-F]{6}$/.test(color)) {
            problem = qsTr("A colour looks like #3366cc.")
            return
        }
        const or = (s) => s.trim() === "" ? null : s.trim()
        const fields = {
            display_name: name,
            pronouns: or(pronounsField.text),
            blurb: or(blurbField.text),
            about: aboutField.text.trim() === "" ? null : aboutField.text,
            avatar: pictures.avatar === "" ? null : pictures.avatar,
            banner: pictures.banner === "" ? null : pictures.banner,
            background: pictures.background === "" ? null : pictures.background
        }
        // a gradient is made in the file; it is kept unless a colour is typed over it
        if (!colorIsGradient || color !== "")
            fields.color = color === "" ? null : color
        const ls = []
        for (let i = 0; i < links.count; i++) {
            const l = links.get(i)
            if (l.text.trim() === "")
                continue
            ls.push(l.url.trim() === "" ? { text: l.text.trim() } : { text: l.text.trim(), url: l.url.trim() })
        }
        fields.links = ls.length ? ls : null
        problem = ""
        saving = true
        chats.saveProfile(isNew ? chats.profileIdFor(name) : profileId, fields, isNew)
    }

    objectName: "profileDialog"
    title: isNew ? qsTr("New profile") : qsTr("Edit profile")
    anchors.centerIn: Overlay.overlay
    modal: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 24 : 560)
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 24 : implicitHeight)
    // what went wrong, and the buttons: below the form, so they are in sight however far it is scrolled
    footer: Pane {
        background: null
        padding: 12
        topPadding: 6
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                visible: dlg.problem !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.alert
                text: dlg.problem
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Button {
                    visible: !dlg.isNew
                    text: qsTr("Delete")
                    palette.buttonText: Theme.danger
                    enabled: !dlg.busy
                    onClicked: confirm.ask(qsTr("Delete this profile?"),
                                           qsTr("Everyone who sees it is told to forget it, and sees only your id after that."),
                                           qsTr("Delete"), () => dlg.chats.deleteProfile(dlg.profileId))
                }
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Cancel"); enabled: !dlg.saving; onClicked: dlg.close() }
                Button {
                    objectName: "profileSave"
                    text: dlg.saving ? qsTr("Saving…") : qsTr("Save")
                    highlighted: true
                    enabled: !dlg.busy && nameField.text.trim() !== ""
                    onClicked: dlg.save()
                }
            }
        }
    }

    Connections {
        target: dlg.chats
        function onProfileSaved(id, error) {
            if (!dlg.saving)
                return
            dlg.saving = false
            if (error !== "")
                dlg.problem = error
            else
                dlg.close()
        }
        function onProfileDeleted(id, error) {
            if (id !== dlg.profileId)
                return
            if (error !== "")
                dlg.problem = error
            else
                dlg.close()
        }
        function onPictureUploaded(slot, hash, error) {
            if (!(slot in dlg.uploading))
                return
            const u = Object.assign({}, dlg.uploading)
            delete u[slot]
            dlg.uploading = u
            if (error !== "")
                dlg.problem = error
            else
                dlg.setPicture(slot, hash)
        }
    }

    FileDialog {
        id: picker
        property string slot: ""
        title: qsTr("Choose a picture")
        nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.gif *.webp)")]
        onAccepted: {
            const u = Object.assign({}, dlg.uploading)
            u[slot] = true
            dlg.uploading = u
            dlg.problem = ""
            dlg.chats.uploadPicture(selectedFile, slot)
        }
    }
    ConfirmDialog { id: confirm }
    ListModel { id: links }

    ScrollArea {
        anchors.fill: parent
        implicitHeight: form.implicitHeight
        contentWidth: width
        contentHeight: form.implicitHeight

        ColumnLayout {
            id: form
            width: parent.width - 12
            spacing: 10

            // ---- the picture and the name ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Avatar {
                    chats: dlg.chats
                    name: nameField.text
                    hash: dlg.pictures.avatar
                    tint: /^#[0-9a-fA-F]{6}$/.test(colorField.text.trim()) ? colorField.text.trim() : ""
                    size: 64
                }
                ColumnLayout {
                    spacing: 4
                    Label { text: qsTr("Picture"); opacity: 0.7 }
                    RowLayout {
                        Button {
                            text: "avatar" in dlg.uploading ? qsTr("Sending…") : qsTr("Choose…")
                            enabled: !("avatar" in dlg.uploading)
                            onClicked: { picker.slot = "avatar"; picker.open() }
                        }
                        Button { visible: dlg.pictures.avatar !== ""; text: qsTr("Remove"); onClicked: dlg.setPicture("avatar", "") }
                    }
                }
            }

            Label { text: qsTr("Name"); opacity: 0.7 }
            TextField {
                id: nameField
                objectName: "profileName"
                Layout.fillWidth: true
                maximumLength: 64
                placeholderText: qsTr("How they see you")
                Accessible.name: qsTr("Name")
                FieldMenu {}
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: qsTr("Pronouns"); opacity: 0.7 }
                    TextField { id: pronounsField; Layout.fillWidth: true; maximumLength: 32; placeholderText: qsTr("they/them"); FieldMenu {} }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label { text: qsTr("Name colour"); opacity: 0.7 }
                    RowLayout {
                        Rectangle {
                            width: 22; height: 22; radius: 11
                            border.width: 1; border.color: Theme.hairline
                            color: /^#[0-9a-fA-F]{6}$/.test(colorField.text.trim()) ? colorField.text.trim() : "transparent"
                        }
                        TextField {
                            id: colorField
                            Layout.fillWidth: true
                            maximumLength: 7
                            font.family: dlg.mono
                            placeholderText: dlg.colorIsGradient ? qsTr("a gradient") : "#3366cc"
                            FieldMenu {}
                        }
                    }
                }
            }
            Label { text: qsTr("Short bio"); opacity: 0.7 }
            TextField { id: blurbField; Layout.fillWidth: true; maximumLength: 160; placeholderText: qsTr("A line about you"); FieldMenu {} }
            Label { text: qsTr("About"); opacity: 0.7 }
            TextArea {
                id: aboutField
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(80, contentHeight + 16)
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("As much as you like, up to 2000 characters")
                onTextChanged: if (length > 2000) remove(2000, length)
            }

            // ---- the banner and the background ----
            Repeater {
                model: [{ slot: "banner", label: qsTr("Banner") }, { slot: "background", label: qsTr("Background") }]
                delegate: RowLayout {
                    id: pic
                    required property var modelData
                    property string url: ""
                    function load() { url = dlg.pictures[modelData.slot] !== "" ? dlg.chats.avatarFor(dlg.pictures[modelData.slot]) : "" }
                    Connections {
                        target: dlg
                        function onPicturesChanged() { pic.load() }
                    }
                    Connections {
                        target: dlg.chats
                        function onAvatarReady(h) { if (h === dlg.pictures[pic.modelData.slot]) pic.load() }
                    }
                    Component.onCompleted: load()
                    Layout.fillWidth: true
                    spacing: 12
                    Rectangle {
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 48
                        radius: Theme.radiusLarge
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.hairline
                        clip: true
                        Image { anchors.fill: parent; source: pic.url; fillMode: Image.PreserveAspectCrop; asynchronous: true }
                        Label { anchors.centerIn: parent; visible: pic.url === ""; text: qsTr("none"); opacity: 0.5 }
                    }
                    Label { text: pic.modelData.label; Layout.fillWidth: true }
                    Button {
                        text: pic.modelData.slot in dlg.uploading ? qsTr("Sending…") : qsTr("Choose…")
                        enabled: !(pic.modelData.slot in dlg.uploading)
                        onClicked: { picker.slot = pic.modelData.slot; picker.open() }
                    }
                    Button {
                        visible: dlg.pictures[pic.modelData.slot] !== ""
                        text: qsTr("Remove")
                        onClicked: dlg.setPicture(pic.modelData.slot, "")
                    }
                }
            }

            // ---- links ----
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Links"); opacity: 0.7; Layout.fillWidth: true }
                Button { text: qsTr("Add a link"); enabled: links.count < 10; onClicked: links.append({ text: "", url: "" }) }
            }
            Repeater {
                model: links
                delegate: RowLayout {
                    required property int index
                    required property string text
                    required property string url
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        Layout.preferredWidth: 180
                        maximumLength: 64
                        text: parent.text
                        placeholderText: qsTr("What it says")
                        onTextEdited: links.setProperty(parent.index, "text", text)
                        FieldMenu {}
                    }
                    TextField {
                        Layout.fillWidth: true
                        maximumLength: 512
                        text: parent.url
                        placeholderText: "https://…"
                        onTextEdited: links.setProperty(parent.index, "url", text)
                        FieldMenu {}
                    }
                    ToolButton { text: "✕"; onClicked: links.remove(parent.index); Accessible.name: qsTr("Remove this link") }
                }
            }

            Label {
                visible: !dlg.isNew
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.6
                font.pixelSize: 12
                readonly property int n: (dlg.original.places || []).length
                text: n === 0 ? qsTr("Nobody sees this profile yet: choose it for a chat, a group or a Pond.")
                              : n === 1 ? qsTr("Seen in 1 place. Everyone there is told when you save.")
                              : qsTr("Seen in %1 places. Everyone there is told when you save.").arg(n)
            }
        }
    }
}
