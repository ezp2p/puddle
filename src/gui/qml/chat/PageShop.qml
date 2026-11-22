import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A shop page (the Pond's docs/design.md, "Shop"): what the Pond sells, or takes as donations. Buying asks the Pond for a Stripe
// link for that order alone and opens it in the browser; the Pond sees the payment and records it. Whoever may manage the shop
// adds, changes and takes items off here; Stripe is connected in Server Settings, Shop. Your orders are under the items.
ColumnLayout {
    id: part
    property var chats
    property var page: ({})
    readonly property var ponds: chats ? chats.ponds : null
    readonly property var items: page.items || []
    readonly property string currency: ((page.data && page.data.currency) || "usd").toUpperCase()
    spacing: 12

    function money(cents) { return (cents / 100).toFixed(2) + " " + currency }
    function mediaUrl(name) {
        const f = (page.files || []).find(x => x.name === name)
        return f && f.state === "available" ? chats.fileUrl(f.id, f.name) : ""
    }
    Component.onCompleted: if (ponds) ponds.loadOrders(false)
    onPageChanged: if (ponds) ponds.loadOrders(false)

    // payments: not set up, or in test mode
    Rectangle {
        visible: part.ponds && (!part.page.payments || part.ponds.shopTest)
        Layout.fillWidth: true
        implicitHeight: payNote.implicitHeight + 20
        radius: Theme.radiusLarge
        color: Theme.tint(Theme.accent, 0.15)
        RowLayout {
            id: payNote
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: !part.page.payments ? (part.ponds.canManagePond ? qsTr("This shop doesn't take payments yet: connect Stripe in Server Settings, Shop.")
                                                                      : qsTr("This shop doesn't take payments yet."))
                                          : qsTr("Payments are in Stripe's test mode: nothing is really charged.")
            }
            Button {
                visible: !part.page.payments && part.ponds.canManagePond
                text: qsTr("Set up")
                onClicked: part.ponds.openSettings("shop")
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("For sale"); font.bold: true; font.pixelSize: 16; Layout.fillWidth: true }
        PrimaryButton {
            objectName: "addItem"
            visible: part.ponds && part.ponds.canManageShop && itemForm.editing === ""
            text: qsTr("Add item")
            onClicked: itemForm.start(null)
        }
    }

    // a new item, or one being changed
    Rectangle {
        id: itemForm
        property string editing: ""
        function start(it) {
            iTitle.text = it ? it.title : ""
            iPrice.text = it ? (it.price / 100).toFixed(2) : ""
            iCustom.checked = it ? it.custom : false
            iStock.text = it && it.stock !== null ? String(it.stock) : ""
            iImage.currentIndex = it ? Math.max(0, iImage.names.indexOf(it.image)) : 0
            iDesc.text = it ? it.description : ""
            editing = it ? String(it.id) : "new"
        }
        readonly property int cents: Math.round(parseFloat(iPrice.text.replace(",", ".")) * 100)
        visible: editing !== ""
        Layout.fillWidth: true
        implicitHeight: iCol.implicitHeight + 24
        radius: Theme.radiusLarge
        color: Theme.tint(Theme.text, Theme.hover)
        border.width: 1
        border.color: Theme.hairline
        ColumnLayout {
            id: iCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            TextField { id: iTitle; objectName: "itemTitle"; Layout.fillWidth: true; placeholderText: qsTr("What it is"); maximumLength: 100 }
            // one field to a line, so the form fits a narrow page
            RowLayout {
                spacing: 8
                Label { text: qsTr("Price"); Layout.preferredWidth: 72 }
                TextField { id: iPrice; objectName: "itemPrice"; Layout.preferredWidth: 100; placeholderText: "5.00" }
                Label { text: part.currency; opacity: 0.7 }
            }
            WrapCheck { id: iCustom; objectName: "itemCustom"; text: qsTr("Pay what you like (the price is the least)") }
            RowLayout {
                spacing: 8
                Label { text: qsTr("How many"); Layout.preferredWidth: 72 }
                TextField { id: iStock; Layout.preferredWidth: 100; placeholderText: qsTr("no limit"); validator: IntValidator { bottom: 0 } }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Picture"); Layout.preferredWidth: 72 }
                ComboBox {
                    id: iImage
                    readonly property var names: [""].concat((part.page.files || []).filter(f => (f.mime || "").startsWith("image/")).map(f => f.name))
                    model: names.map(n => n === "" ? qsTr("none") : n)
                    Layout.fillWidth: true
                    Layout.maximumWidth: 240
                }
            }
            TextArea {
                id: iDesc
                Layout.fillWidth: true
                Layout.minimumHeight: 70
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("About it (markdown)")
                background: Rectangle { radius: Theme.radius; color: Theme.card; border.width: 1; border.color: Theme.hairline }
            }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Pictures come from the page's media (Edit, Media)."); opacity: 0.6; font.pixelSize: 12 }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Cancel"); onClicked: itemForm.editing = "" }
                PrimaryButton {
                    objectName: "saveItem"
                    text: itemForm.editing === "new" ? qsTr("Add") : qsTr("Save")
                    enabled: iTitle.text.trim() !== "" && !isNaN(itemForm.cents) && itemForm.cents >= 0
                    onClicked: {
                        const f = { title: iTitle.text.trim(), price: itemForm.cents, custom: iCustom.checked, description: iDesc.text,
                                    image: iImage.names[iImage.currentIndex] || "", stock: iStock.text === "" ? null : parseInt(iStock.text) }
                        part.ponds.saveItem(part.page.id, itemForm.editing === "new" ? "" : itemForm.editing, f)
                        itemForm.editing = ""
                    }
                }
            }
        }
    }

    Label { visible: part.items.length === 0; text: qsTr("Nothing for sale yet."); opacity: 0.6 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: part.items
            delegate: Rectangle {
                id: card
                required property var modelData
                readonly property var it: modelData
                readonly property bool soldOut: it.stock !== null && it.stock === 0
                objectName: "item_" + it.title
                width: Math.min(260, part.width)
                implicitHeight: cardCol.implicitHeight + 20
                radius: Theme.radiusLarge
                color: Theme.tint(Theme.text, Theme.hover)
                border.width: 1
                border.color: Theme.hairline
                opacity: it.active ? 1 : 0.55
                ColumnLayout {
                    id: cardCol
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    Rectangle {
                        visible: card.it.image !== ""
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        radius: Theme.radius
                        clip: true
                        color: Theme.tint(Theme.text, Theme.selected)
                        Image {
                            anchors.fill: parent
                            source: part.mediaUrl(card.it.image)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                    Label { text: card.it.title; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Label {
                        text: (card.it.custom ? qsTr("From %1").arg(part.money(card.it.price)) : part.money(card.it.price)) +
                              (card.it.stock !== null ? "  ·  " + (card.soldOut ? qsTr("sold out") : qsTr("%1 left").arg(card.it.stock)) : "") +
                              (card.it.active ? "" : "  ·  " + qsTr("off sale"))
                        opacity: 0.75
                    }
                    Text {
                        visible: card.it.description !== ""
                        Layout.fillWidth: true
                        textFormat: Text.MarkdownText
                        text: card.it.description
                        color: palette.text
                        wrapMode: Text.Wrap
                        font.pixelSize: 13
                    }
                    RowLayout {
                        spacing: 6
                        SpinBox { id: qty; visible: !card.it.custom; from: 1; to: card.it.stock !== null ? Math.max(1, Math.min(100, card.it.stock)) : 100; value: 1; editable: true; Layout.preferredWidth: 110 }
                        TextField {
                            id: amount
                            visible: card.it.custom
                            Layout.preferredWidth: 90
                            placeholderText: (card.it.price / 100).toFixed(2)
                        }
                        Item { Layout.fillWidth: true }
                        PrimaryButton {
                            objectName: "buy_" + card.it.title
                            text: card.it.custom ? qsTr("Give") : qsTr("Buy")
                            enabled: card.it.active && !card.soldOut && part.page.payments
                            onClicked: {
                                const cents = card.it.custom ? Math.round(parseFloat((amount.text || amount.placeholderText).replace(",", ".")) * 100) : 0
                                part.ponds.buy(part.page.id, String(card.it.id), card.it.custom ? 1 : qty.value, cents)
                            }
                        }
                    }
                    RowLayout {
                        visible: part.ponds.canManageShop
                        spacing: 4
                        Button { text: qsTr("Change"); onClicked: itemForm.start(card.it) }
                        Button { text: card.it.active ? qsTr("Off sale") : qsTr("On sale"); onClicked: part.ponds.saveItem(part.page.id, String(card.it.id), { active: !card.it.active }) }
                        ToolButton {
                            icon.source: Theme.icon("trash"); icon.width: 14; icon.height: 14; icon.color: Theme.danger
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("Take %1 off the page").arg(card.it.title)
                            onClicked: part.ponds.deleteItem(part.page.id, String(card.it.id))
                        }
                    }
                }
            }
        }
    }

    // your orders here
    Label {
        visible: part.ponds && part.ponds.orders.count > 0
        text: qsTr("Your orders")
        font.bold: true
        Layout.topMargin: 8
    }
    Repeater {
        model: part.ponds ? part.ponds.orders : null
        delegate: RowLayout {
            required property var model
            Layout.fillWidth: true
            spacing: 8
            Label { text: model.quantity + " × " + model.title; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { text: (model.amount / 100).toFixed(2) + " " + model.currency.toUpperCase(); opacity: 0.75 }
            Label {
                text: model.status === "paid" ? qsTr("paid") : model.status === "open" ? qsTr("waiting for payment") : model.status === "lapsed" ? qsTr("not paid") : qsTr("failed")
                color: model.status === "paid" ? Theme.primary : palette.text
                opacity: model.status === "paid" ? 1 : 0.65
            }
            Button { visible: model.status === "open" && (model.url || "") !== ""; text: qsTr("Pay"); onClicked: Qt.openUrlExternally(model.url) }
        }
    }
}
