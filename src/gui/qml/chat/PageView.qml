import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Puddle.Files
import "../components"
import "../theme"

// One of a Pond's pages (the Pond's docs/design.md, "Pages"), shown where a channel's messages are while it is open: its markdown,
// with each #channel-name a link that opens that channel and each picture of its media shown where the markdown names it
// (![](media:name.png)); then what its kind adds (roles to pick, events, the entry form, the shop); then its other files.
// Whoever may manage pages edits it here, in tabs: the page (title, kind, markdown), its media (each with Insert, to place it in
// the markdown), the kind's settings, and who sees it.
Pane {
    id: view

    objectName: "pageView"
    required property var chats
    readonly property var ponds: chats.ponds
    readonly property var page: ponds.pageData
    property bool sidebarShown: true
    signal toggleSidebar()
    property bool contextShown: false
    signal toggleContext()

    padding: 0
    background: null

    readonly property var kinds: [
        { id: "markdown", title: qsTr("Page"), note: qsTr("Markdown, pictures and files.") },
        { id: "roles", title: qsTr("Roles to pick"), note: qsTr("Roles members take for themselves, to see channels meant for them.") },
        { id: "events", title: qsTr("Events"), note: qsTr("A calendar: those who may manage events add them; members say they're going.") },
        { id: "form", title: qsTr("Entry form"), note: qsTr("Questions those joining answer before they get in.") },
        { id: "shop", title: qsTr("Shop"), note: qsTr("Things to buy, or donations, paid through Stripe.") }
    ]
    function kindTitle(id) { for (const k of kinds) if (k.id === id) return k.title; return id }
    function isImage(f) { return (f.mime || "").startsWith("image/") }

    // ---- reading: pictures in the markdown ----
    property int rev: 0 // a picture came: draw it again
    Connections {
        target: view.chats
        function onFileReady(id) { if ((view.page.files || []).some(f => f.id === id)) view.rev++ }
    }
    // the markdown with each media:NAME picture in place: here, a picture as wide as it fits; coming, a line that says so
    function shown(md, files, width, rev) {
        if (!md)
            return ""
        return md.replace(/!\[([^\]]*)\]\(media:([^)\s]+)\)/g, (all, alt, name) => {
            const f = (files || []).find(x => x.name === decodeURIComponent(name))
            if (!f)
                return "*" + qsTr("(no picture called %1)").arg(decodeURIComponent(name)) + "*"
            if (f.state !== "available") {
                if (f.state === "waiting" || f.state === "failed")
                    view.ponds.fetchPageFile(f.id)
                return "*" + qsTr("(%1 is on its way)").arg(f.name) + "*"
            }
            const url = view.chats.fileUrl(f.id, f.name)
            if (!url)
                return "*" + qsTr("(%1 is on its way)").arg(f.name) + "*"
            const w = Math.min(f.w > 0 ? f.w : width, width)
            const h = f.w > 0 && f.h > 0 ? Math.round(w * f.h / f.w) : 0
            return "<img src=\"" + url + "\" width=\"" + Math.round(w) + "\"" + (h ? " height=\"" + h + "\"" : "") + " alt=\"" + (alt || f.name).replace(/"/g, "") + "\">"
        })
    }
    // the files under the page: all but the pictures, which show only where the markdown places them
    readonly property var attached: (page.files || []).filter(f => !isImage(f))

    // ---- editing ----
    property bool editing: false
    property var keep: []          // the files it carries now, less those taken off
    property string editKind: "markdown"
    property var editData: ({})
    property var editRoles: null   // null: everyone; else role ids
    function startEdit() {
        titleField.text = page.title || ""
        bodyField.text = page.body || ""
        keep = (page.files || []).slice()
        editKind = page.kind || "markdown"
        editData = JSON.parse(JSON.stringify(page.data || {}))
        editRoles = page.roles === undefined || page.roles === null ? null : page.roles.map(r => String(r))
        composer.clear()
        editTabs.currentIndex = 0
        editing = true
        bodyField.forceActiveFocus()
    }
    function plain(f) { return f.w > 0 && f.h > 0 ? { id: f.id, name: f.name, mime: f.mime, size: f.size, w: f.w, h: f.h } : { id: f.id, name: f.name, mime: f.mime, size: f.size } }
    function extra() {
        const e = { kind: editKind, data: editData, roles: editRoles === null ? null : editRoles.map(r => Number(r)) }
        return e
    }
    function save() {
        if (titleField.text.trim() === "")
            return
        if (composer.count > 0) { // new files: sent to our Puddle, then to the Pond with the page
            const e = extra()
            e.title = titleField.text.trim()
            e.body = bodyField.text
            composer.sendWith(e, keep.map(f => plain(f)))
        } else {
            ponds.savePage(page.id, titleField.text, bodyField.text, keep, extra())
        }
    }
    // put text in the markdown where the cursor is
    function insert(text) {
        const at = bodyField.cursorPosition
        bodyField.insert(at, text)
        editTabs.currentIndex = 0
        bodyField.forceActiveFocus()
        bodyField.cursorPosition = at + text.length
    }
    Connections {
        target: view.ponds
        function onPageChanged() { view.editing = false }
        function onPageSaved() { view.editing = false }
    }
    Composer {
        id: composer
        chats: view.chats
        target: view.ponds.page !== "" ? "/api/v1/ponds/" + view.ponds.current + "/admin/pages/" + view.ponds.page + "/edit" : ""
        onRefused: why => view.chats.failed(why)
        onFailed: why => view.chats.failed(why)
        onSent: { view.editing = false; view.ponds.refresh() }
    }
    FileDialog {
        id: picker
        title: qsTr("Add pictures or files")
        fileMode: FileDialog.OpenFiles
        onAccepted: composer.add(selectedFiles)
    }
    FileActions {
        id: fileActions
        chats: view.chats
        onFailed: why => view.chats.failed(why)
    }
    // a file the page carries that did not come by itself: asked of the Pond
    QtObject {
        id: pageFetcher
        function fetchFile(messageId, fileId) { view.ponds.fetchPageFile(fileId) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.headerHeight
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                PanelToggle { glyph: "layout-sidebar"; label: qsTr("Channels"); active: view.sidebarShown; onClicked: view.toggleSidebar() }
                Icon { glyph: "file-text"; size: 20; opacity: 0.6 }
                Label { objectName: "pageTitle"; text: view.page.title || ""; font.bold: true; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                Icon {
                    visible: view.page.roles !== undefined && view.page.roles !== null
                    glyph: "lock"; size: 16; opacity: 0.6
                    HoverHandler { id: lockHover }
                    ToolTip.visible: lockHover.hovered
                    ToolTip.text: qsTr("Only some roles see this page")
                }
                // for whoever may manage pages
                ToolButton {
                    objectName: "pageMoveUp"
                    visible: view.ponds.canManagePages && !view.editing
                    enabled: view.ponds.pages.count > 1
                    icon.source: Theme.icon("arrow-up"); icon.width: 16; icon.height: 16
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Move up")
                    ToolTip.text: qsTr("Move up"); ToolTip.visible: hovered
                    onClicked: view.ponds.movePage(view.ponds.page, -1)
                }
                ToolButton {
                    objectName: "pageMoveDown"
                    visible: view.ponds.canManagePages && !view.editing
                    enabled: view.ponds.pages.count > 1
                    icon.source: Theme.icon("arrow-down"); icon.width: 16; icon.height: 16
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Move down")
                    ToolTip.text: qsTr("Move down"); ToolTip.visible: hovered
                    onClicked: view.ponds.movePage(view.ponds.page, 1)
                }
                ToolButton {
                    objectName: "pageEdit"
                    visible: view.ponds.canManagePages && !view.editing
                    enabled: view.page.id !== undefined
                    icon.source: Theme.icon("pencil"); icon.width: 16; icon.height: 16
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Edit page")
                    ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered
                    onClicked: view.startEdit()
                }
                ToolButton {
                    objectName: "pageDelete"
                    visible: view.ponds.canManagePages && !view.editing
                    icon.source: Theme.icon("trash"); icon.width: 16; icon.height: 16; icon.color: Theme.danger
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Delete page")
                    ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered
                    onClicked: confirm.ask(qsTr("Delete %1?").arg(view.page.title || ""), qsTr("It goes for everyone, with the files it carries."),
                                           qsTr("Delete page"), (id => () => view.ponds.deletePage(id))(view.ponds.page))
                }
                PanelToggle { glyph: "layout-sidebar-right"; label: qsTr("Members"); active: view.contextShown; onClicked: view.toggleContext() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        }

        ScrollArea {
            id: scroller
            objectName: "pageScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: content.implicitHeight + 48
            ColumnLayout {
                id: content
                x: Math.min(32, Math.max(12, (scroller.width - width) / 2))
                y: 24
                width: Math.min(820, scroller.width - 2 * Math.min(32, scroller.width / 20 + 8))
                spacing: 14

                // ---- reading it ----
                Label {
                    visible: !view.editing && view.page.id === undefined
                    text: qsTr("Loading…")
                    opacity: 0.6
                }
                Label {
                    visible: !view.editing && view.page.id !== undefined && (view.page.body || "") === "" && (view.page.files || []).length === 0 &&
                             (view.page.kind || "markdown") === "markdown"
                    text: view.ponds.canManagePages ? qsTr("This page is empty. Edit it to write something.") : qsTr("This page is empty.")
                    opacity: 0.6
                }
                Text {
                    id: rendered
                    objectName: "pageText"
                    visible: !view.editing && (view.page.body || "") !== ""
                    Layout.fillWidth: true
                    textFormat: Text.MarkdownText
                    text: view.shown(view.page.markdown || "", view.page.files, content.width, view.rev)
                    wrapMode: Text.Wrap
                    color: palette.text
                    linkColor: Theme.primary
                    font.pixelSize: 15
                    // #channel-name opens it; a web link opens in the browser; nothing else
                    onLinkActivated: link => {
                        if (link.startsWith("channel:"))
                            view.ponds.openChannel(link.substring(8))
                        else if (/^https?:\/\//.test(link))
                            Qt.openUrlExternally(link)
                    }
                    HoverHandler { cursorShape: rendered.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor }
                }
                // what the kind adds
                Loader {
                    id: kindPart
                    objectName: "pageKindPart"
                    active: !view.editing && view.page.id !== undefined && (view.page.kind || "markdown") !== "markdown"
                    visible: active
                    Layout.fillWidth: true
                    source: {
                        switch (view.page.kind) {
                        case "roles": return "PageRoles.qml"
                        case "events": return "PageEvents.qml"
                        case "form": return "PageForm.qml"
                        case "shop": return "PageShop.qml"
                        }
                        return ""
                    }
                    onLoaded: { item.chats = view.chats; item.page = Qt.binding(() => view.page) }
                }
                MessageFiles {
                    visible: !view.editing && view.attached.length > 0
                    Layout.fillWidth: true
                    chats: view.chats
                    fetcher: pageFetcher
                    files: view.attached
                    messageId: "page-" + (view.page.id || "")
                    onViewMedia: (list, i) => fileActions.view(list, i)
                    onSaveFile: f => fileActions.save(f)
                    onOpenFile: f => fileActions.open(f)
                }
                Label {
                    visible: !view.editing && (view.page.updated || 0) > 0
                    text: qsTr("Last changed by %1, %2").arg(view.page.updatedBy || "").arg(new Date(view.page.updated || 0).toLocaleString(Qt.locale(), Locale.ShortFormat))
                    font.pixelSize: 11
                    opacity: 0.5
                }

                // ---- editing it: the title, with Cancel and Save beside it, always in view ----
                RowLayout {
                    visible: view.editing
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: titleField
                        objectName: "pageTitleField"
                        Layout.fillWidth: true
                        maximumLength: 64
                        placeholderText: qsTr("Title")
                        font.bold: true
                        font.pixelSize: 18
                    }
                    Button { text: qsTr("Cancel"); enabled: !composer.sending; onClicked: { composer.clear(); view.editing = false } }
                    PrimaryButton {
                        objectName: "pageSave"
                        text: composer.sending ? qsTr("Saving…") : qsTr("Save")
                        enabled: titleField.text.trim() !== "" && !composer.sending && (composer.count === 0 || composer.ready)
                        onClicked: view.save()
                    }
                }
                TabStrip {
                    id: editTabs
                    objectName: "pageEditTabs"
                    visible: view.editing
                    tabs: [{ text: qsTr("Page"), name: "pageTabContent" }, { text: qsTr("Media (%1)").arg(view.keep.length + composer.count), name: "pageTabMedia" },
                           { text: view.editKind === "roles" ? qsTr("Roles") : view.editKind === "form" ? qsTr("Questions") : view.editKind === "shop" ? qsTr("Shop") : qsTr("Kind"), name: "pageTabKind" },
                           { text: qsTr("Who sees it"), name: "pageTabAccess" }]
                }
                StackLayout {
                    visible: view.editing
                    Layout.fillWidth: true
                    currentIndex: editTabs.currentIndex

                    // the page: its markdown
                    ColumnLayout {
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            opacity: 0.6
                            text: qsTr("Markdown: # for a heading, **bold**, *italic*, - for a list, [words](https://…) for a link. #channel-name becomes a link to that channel. A picture from Media shows where you put ![](media:its-name), which Insert writes for you.")
                        }
                        TextArea {
                            id: bodyField
                            objectName: "pageBodyField"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 320
                            wrapMode: TextEdit.Wrap
                            font.family: "monospace"
                            placeholderText: qsTr("# Welcome\n\nWhat this Pond is about, its rules, where to start…")
                            background: Rectangle { radius: Theme.radius; color: Theme.tint(Theme.text, Theme.hover); border.width: 1; border.color: Theme.hairline }
                        }
                    }

                    // its media: what it carries, and new files to send; each picture can be placed in the markdown
                    ColumnLayout {
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            opacity: 0.6
                            text: qsTr("Pictures show only where the page's markdown names them: Insert puts one at the cursor. Other files show under the page. Up to 10.")
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 10
                            Repeater {
                                model: view.editing ? view.keep : []
                                delegate: MediaTile {
                                    required property var modelData
                                    required property int index
                                    objectName: "pageMedia_" + modelData.name
                                    chats: view.chats
                                    name: modelData.name
                                    image: view.isImage(modelData)
                                    url: image && modelData.state === "available" ? view.chats.fileUrl(modelData.id, modelData.name) : ""
                                    onInsert: view.insert(image ? "![](media:" + encodeURIComponent(modelData.name) + ")" : "[" + modelData.name + "](media:" + encodeURIComponent(modelData.name) + ")")
                                    onRemove: { const k = view.keep.slice(); k.splice(index, 1); view.keep = k }
                                }
                            }
                            Repeater {
                                model: view.editing ? composer.items : null
                                delegate: MediaTile {
                                    required property var model
                                    objectName: "pageMedia_" + model.name
                                    chats: view.chats
                                    name: model.name
                                    image: (model.mime || "").startsWith("image/")
                                    url: image ? (model.thumb || model.url || "") : ""
                                    fresh: true
                                    onInsert: view.insert(image ? "![](media:" + encodeURIComponent(model.name) + ")" : "[" + model.name + "](media:" + encodeURIComponent(model.name) + ")")
                                    onRemove: composer.remove(model.key)
                                }
                            }
                        }
                        Button {
                            objectName: "pageAttach"
                            text: qsTr("Add pictures or files")
                            icon.source: Theme.icon("paperclip"); icon.width: 16; icon.height: 16
                            enabled: !composer.sending && view.keep.length + composer.count < 10
                            onClicked: picker.open()
                        }
                    }

                    // the kind, and its settings
                    ColumnLayout {
                        spacing: 10
                        Label { text: qsTr("What this page is"); font.bold: true }
                        Repeater {
                            model: view.ponds.canPageKinds ? view.kinds : view.kinds.slice(0, 1)
                            delegate: RadioButton {
                                id: kindChoice
                                required property var modelData
                                objectName: "pageKind_" + modelData.id
                                Layout.fillWidth: true
                                text: modelData.title + " — " + modelData.note
                                checked: view.editKind === modelData.id
                                onClicked: { if (view.editKind !== modelData.id) { view.editKind = modelData.id; view.editData = ({}) } }
                                // a long line wraps, so the page never needs more width than it has
                                contentItem: Label {
                                    leftPadding: kindChoice.indicator.width + kindChoice.spacing
                                    text: kindChoice.text
                                    wrapMode: Text.Wrap
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                        Loader {
                            Layout.fillWidth: true
                            active: view.editing && ["roles", "form", "shop"].includes(view.editKind)
                            source: view.editKind === "roles" ? "PageRolesSettings.qml" : view.editKind === "form" ? "PageFormSettings.qml" : view.editKind === "shop" ? "PageShopSettings.qml" : ""
                            onLoaded: { item.ponds = view.ponds; item.data = view.editData; item.changed.connect(d => view.editData = d) }
                        }
                    }

                    // who sees it
                    ColumnLayout {
                        spacing: 8
                        WrapRadio {
                            objectName: "pageForEveryone"
                            text: qsTr("Everyone in the Pond")
                            checked: view.editRoles === null
                            onClicked: view.editRoles = null
                        }
                        WrapRadio {
                            objectName: "pageForRoles"
                            text: qsTr("Only some roles (and those who may manage pages)")
                            checked: view.editRoles !== null
                            enabled: view.ponds.roles.count > 0
                            onClicked: if (view.editRoles === null) view.editRoles = []
                        }
                        Flow {
                            visible: view.editRoles !== null
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            spacing: 6
                            Repeater {
                                model: view.ponds.roles
                                delegate: CheckBox {
                                    required property var model
                                    objectName: "pageRole_" + model.name
                                    text: model.name
                                    checked: view.editRoles !== null && view.editRoles.includes(model.id)
                                    onToggled: {
                                        const r = (view.editRoles || []).filter(x => x !== model.id)
                                        if (checked) r.push(model.id)
                                        view.editRoles = r
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ConfirmDialog { id: confirm }
}
