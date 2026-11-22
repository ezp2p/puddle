import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Puddle.Files
import "../components"
import "../theme"

// One of a Pond's pages (the Pond's docs/design.md, "Pages"), shown where a channel's messages are while it is open: its markdown,
// with each #channel-name a link that opens that channel, and the files it carries under it, as a message's. Whoever may manage
// pages edits it here: its title, its markdown, and its files (those it carries, and new ones chosen with the paperclip, sent to
// the Pond before the page names them); and moves it up or down, or deletes it.
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

    property bool editing: false
    property var keep: []          // while editing: the files it carries now, less those taken off
    function startEdit() {
        titleField.text = page.title || ""
        bodyField.text = page.body || ""
        keep = (page.files || []).slice()
        composer.clear()
        editing = true
        bodyField.forceActiveFocus()
    }
    function save() {
        if (titleField.text.trim() === "")
            return
        if (composer.count > 0) { // new files: sent to our Puddle, then to the Pond with the page
            composer.sendWith({ title: titleField.text.trim(), body: bodyField.text },
                              keep.map(f => (f.w > 0 && f.h > 0 ? { id: f.id, name: f.name, mime: f.mime, size: f.size, w: f.w, h: f.h }
                                                                  : { id: f.id, name: f.name, mime: f.mime, size: f.size })))
        } else {
            ponds.savePage(page.id, titleField.text, bodyField.text, keep)
        }
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
        title: qsTr("Attach files")
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
                x: 32
                y: 24
                width: Math.min(820, scroller.width - 64)
                spacing: 14

                // ---- reading it ----
                Label {
                    visible: !view.editing && view.page.id === undefined
                    text: qsTr("Loading…")
                    opacity: 0.6
                }
                Label {
                    visible: !view.editing && view.page.id !== undefined && (view.page.body || "") === "" && (view.page.files || []).length === 0
                    text: view.ponds.canManagePages ? qsTr("This page is empty. Edit it to write something.") : qsTr("This page is empty.")
                    opacity: 0.6
                }
                Text {
                    id: rendered
                    objectName: "pageText"
                    visible: !view.editing && (view.page.body || "") !== ""
                    Layout.fillWidth: true
                    textFormat: Text.MarkdownText
                    text: view.page.markdown || ""
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
                MessageFiles {
                    visible: !view.editing && (view.page.files || []).length > 0
                    Layout.fillWidth: true
                    chats: view.chats
                    fetcher: pageFetcher
                    files: view.page.files || []
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

                // ---- editing it ----
                TextField {
                    id: titleField
                    objectName: "pageTitleField"
                    visible: view.editing
                    Layout.fillWidth: true
                    maximumLength: 64
                    placeholderText: qsTr("Title")
                    font.bold: true
                    font.pixelSize: 18
                }
                Label {
                    visible: view.editing
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    opacity: 0.6
                    text: qsTr("Markdown: # for a heading, **bold**, *italic*, - for a list, [words](https://…) for a link. #channel-name becomes a link to that channel. Files attached show under the page.")
                }
                TextArea {
                    id: bodyField
                    objectName: "pageBodyField"
                    visible: view.editing
                    Layout.fillWidth: true
                    Layout.minimumHeight: 320
                    wrapMode: TextEdit.Wrap
                    font.family: "monospace"
                    placeholderText: qsTr("# Welcome\n\nWhat this Pond is about, its rules, where to start…")
                    background: Rectangle { radius: Theme.radius; color: Theme.tint(Theme.text, Theme.hover); border.width: 1; border.color: Theme.hairline }
                }
                // the files: those it carries (each can be taken off), and new ones to send
                Flow {
                    visible: view.editing
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: view.editing ? view.keep : []
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            height: 28; radius: 14
                            width: keptRow.implicitWidth + 16
                            color: Theme.tint(Theme.text, Theme.selected)
                            Row {
                                id: keptRow
                                anchors.centerIn: parent
                                spacing: 4
                                Icon { anchors.verticalCenter: parent.verticalCenter; glyph: parent.parent.modelData.mime.startsWith("image/") ? "photo" : "file"; size: 14 }
                                Label { anchors.verticalCenter: parent.verticalCenter; text: parent.parent.modelData.name; font.pixelSize: 12 }
                                ToolButton {
                                    implicitWidth: 20; implicitHeight: 20; padding: 2
                                    icon.source: Theme.icon("x"); icon.width: 12; icon.height: 12
                                    display: AbstractButton.IconOnly
                                    Accessible.name: qsTr("Take %1 off").arg(parent.parent.modelData.name)
                                    onClicked: { const k = view.keep.slice(); k.splice(parent.parent.index, 1); view.keep = k }
                                }
                            }
                        }
                    }
                    Repeater {
                        model: view.editing ? composer.items : null
                        delegate: Rectangle {
                            required property var model
                            height: 28; radius: 14
                            width: newRow.implicitWidth + 16
                            color: Theme.tint(Theme.accent, 0.18)
                            Row {
                                id: newRow
                                anchors.centerIn: parent
                                spacing: 4
                                Icon { anchors.verticalCenter: parent.verticalCenter; glyph: "paperclip"; size: 14 }
                                Label { anchors.verticalCenter: parent.verticalCenter; text: parent.parent.model.name; font.pixelSize: 12 }
                                ToolButton {
                                    implicitWidth: 20; implicitHeight: 20; padding: 2
                                    icon.source: Theme.icon("x"); icon.width: 12; icon.height: 12
                                    display: AbstractButton.IconOnly
                                    Accessible.name: qsTr("Take %1 off").arg(parent.parent.model.name)
                                    onClicked: composer.remove(parent.parent.model.key)
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    visible: view.editing
                    spacing: 8
                    Button {
                        objectName: "pageAttach"
                        text: qsTr("Attach files")
                        icon.source: Theme.icon("paperclip"); icon.width: 16; icon.height: 16
                        enabled: !composer.sending
                        onClicked: picker.open()
                    }
                    Item { Layout.fillWidth: true }
                    Button { text: qsTr("Cancel"); enabled: !composer.sending; onClicked: { composer.clear(); view.editing = false } }
                    PrimaryButton {
                        objectName: "pageSave"
                        text: composer.sending ? qsTr("Saving…") : qsTr("Save")
                        enabled: titleField.text.trim() !== "" && !composer.sending && (composer.count === 0 || composer.ready)
                        onClicked: view.save()
                    }
                }
            }
        }
    }

    ConfirmDialog { id: confirm }
}
