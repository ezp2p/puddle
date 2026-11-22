import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Server Settings, Shop (the Pond's docs/design.md, "Shop"): Stripe, and the orders. The Pond takes payments with a Stripe key
// its admins give it (a restricted key with Payment Links, Prices and Checkout Sessions is enough): it makes a link for each
// order and sees the payment itself, so nothing of the Pond need be public. The key is kept by the Pond and never shown again.
ColumnLayout {
    id: pane
    required property var settings
    readonly property var ponds: settings.ponds
    spacing: 16
    Component.onCompleted: if (ponds.canManageShop) ponds.loadOrders(true)

    SettingsGroup {
        title: qsTr("Stripe")
        visible: pane.ponds.canManagePond
        Layout.fillWidth: true
        note: qsTr("Stripe takes the card, checks the buyer and pays the money out; the Pond keeps the items and the orders. Make a key in Stripe's dashboard, under Developers, API keys: a restricted key with write access to Prices, Payment Links and read access to Checkout Sessions. A test key (sk_test_ or rk_test_) charges nothing.")
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: pane.ponds.shopPayments ? (pane.ponds.shopTest ? qsTr("Connected, in test mode") : qsTr("Connected: taking payments")) : qsTr("Not connected")
                color: pane.ponds.shopPayments ? Theme.primary : palette.text
                font.bold: true
                Layout.fillWidth: true
            }
            Button {
                visible: pane.ponds.shopPayments
                text: qsTr("Disconnect")
                onClicked: pane.settings.confirmAsk(qsTr("Stop taking payments?"), qsTr("The Pond forgets the key. Orders already paid stay recorded."),
                                                    qsTr("Disconnect"), () => pane.ponds.setStripeKey(""))
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: key
                objectName: "stripeKey"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: pane.ponds.shopPayments ? qsTr("a new key, to replace it") : "sk_… or rk_…"
            }
            PrimaryButton {
                objectName: "saveStripeKey"
                text: qsTr("Connect")
                enabled: key.text.trim().length > 20
                onClicked: { pane.ponds.setStripeKey(key.text); key.text = "" }
            }
        }
    }

    SettingsGroup {
        title: qsTr("Orders (%1)").arg(pane.ponds.orders.count)
        visible: pane.ponds.canManageShop
        Layout.fillWidth: true
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("The newest 200"); opacity: 0.6; font.pixelSize: 12; Layout.fillWidth: true }
            Button { text: qsTr("Refresh"); onClicked: pane.ponds.loadOrders(true) }
        }
        Label { visible: pane.ponds.orders.count === 0; text: qsTr("No orders yet."); opacity: 0.6 }
        Repeater {
            model: pane.ponds.orders
            delegate: RowLayout {
                required property var model
                Layout.fillWidth: true
                spacing: 10
                Label { text: new Date(model.created).toLocaleDateString(Qt.locale(), Locale.ShortFormat); opacity: 0.6; Layout.preferredWidth: 80 }
                Label { text: model.memberName; Layout.preferredWidth: 120; elide: Text.ElideRight }
                Label { text: model.quantity + " × " + model.title; Layout.fillWidth: true; elide: Text.ElideRight }
                Label { text: (model.amount / 100).toFixed(2) + " " + model.currency.toUpperCase() + (model.live ? "" : " " + qsTr("(test)")) }
                Label {
                    Layout.preferredWidth: 90
                    text: model.status === "paid" ? qsTr("paid") : model.status === "open" ? qsTr("waiting") : model.status === "lapsed" ? qsTr("not paid") : qsTr("failed")
                    color: model.status === "paid" ? Theme.primary : palette.text
                }
            }
        }
    }
}
