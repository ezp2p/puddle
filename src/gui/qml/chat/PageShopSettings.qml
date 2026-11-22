import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// A shop page's settings in its editor: the currency its prices are in. Items are added on the page itself; Stripe is
// connected in Server Settings, Shop.
ColumnLayout {
    id: s
    property var ponds
    property var data: ({})
    signal changed(var data)
    spacing: 8

    Label { text: qsTr("Currency"); font.bold: true }
    RowLayout {
        spacing: 8
        TextField {
            objectName: "shopCurrency"
            Layout.preferredWidth: 80
            maximumLength: 3
            text: (s.data.currency || "usd").toUpperCase()
            onTextEdited: { const d = { currency: text.toLowerCase() }; s.data = d; s.changed(d) }
        }
        Label { text: qsTr("as Stripe names it: USD, EUR, GBP, AUD…"); opacity: 0.6; font.pixelSize: 12 }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.6
        text: qsTr("Items are added on the page once it is saved. The Pond takes payments once Stripe is connected in Server Settings, Shop.")
    }
}
