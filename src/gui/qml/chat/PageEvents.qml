import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// An events page (the Pond's docs/design.md, "Pages"): a calendar, soonest first (and the last month's, under them). Members say
// they're going; whoever may manage events adds, changes and deletes them here.
ColumnLayout {
    id: part
    property var chats
    property var page: ({})
    readonly property var ponds: chats ? chats.ponds : null
    readonly property var events: page.events || []
    readonly property double now: Date.now()
    readonly property var coming: events.filter(e => (e.ends || e.starts) >= now)
    readonly property var past: events.filter(e => (e.ends || e.starts) < now).reverse()
    spacing: 12

    property string editing: ""  // the event being changed; "new" for a new one; "" for none
    function startEdit(e) {
        const d = e ? new Date(e.starts) : new Date(Date.now() + 86400000)
        if (!e)
            d.setMinutes(0, 0, 0)
        title.text = e ? e.title : ""
        day.text = Qt.formatDate(d, "yyyy-MM-dd")
        from.text = Qt.formatTime(d, "HH:mm")
        to.text = e && e.ends ? Qt.formatTime(new Date(e.ends), "HH:mm") : ""
        place.text = e ? e.place : ""
        description.text = e ? e.description : ""
        editing = e ? String(e.id) : "new"
    }
    // a day and a time, as the fields say them: ms, or NaN
    function at(dayText, timeText) {
        const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dayText.trim()), t = /^(\d{1,2}):(\d{2})$/.exec(timeText.trim())
        if (!m || !t)
            return NaN
        return new Date(+m[1], +m[2] - 1, +m[3], +t[1], +t[2]).getTime()
    }
    readonly property double startsAt: at(day.text, from.text)
    readonly property double endsAt: to.text.trim() === "" ? 0 : at(day.text, to.text)
    function save() {
        let ends = endsAt
        if (ends && ends < startsAt)
            ends += 86400000 // past midnight
        const f = { title: title.text.trim(), starts: startsAt, ends: ends || null, place: place.text.trim(), description: description.text }
        ponds.saveEvent(page.id, editing === "new" ? "" : editing, f)
        editing = ""
    }
    function when(e) {
        const s = new Date(e.starts)
        let out = s.toLocaleDateString(Qt.locale(), "dddd d MMMM yyyy") + ", " + Qt.formatTime(s, "HH:mm")
        if (e.ends)
            out += " – " + Qt.formatTime(new Date(e.ends), "HH:mm")
        return out
    }

    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("Coming up"); font.bold: true; font.pixelSize: 16; Layout.fillWidth: true }
        PrimaryButton {
            objectName: "addEvent"
            visible: part.ponds && part.ponds.canManageEvents && part.editing === ""
            text: qsTr("Add event")
            onClicked: part.startEdit(null)
        }
    }

    // the form, for a new event or one being changed
    Rectangle {
        visible: part.editing !== ""
        Layout.fillWidth: true
        implicitHeight: form.implicitHeight + 24
        radius: Theme.radiusLarge
        color: Theme.tint(Theme.text, Theme.hover)
        border.width: 1
        border.color: Theme.hairline
        ColumnLayout {
            id: form
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            TextField { id: title; objectName: "eventTitle"; Layout.fillWidth: true; placeholderText: qsTr("What"); maximumLength: 100 }
            RowLayout {
                spacing: 8
                TextField { id: day; objectName: "eventDay"; Layout.preferredWidth: 130; placeholderText: "2026-12-31"; inputMask: "9999-99-99" }
                TextField { id: from; objectName: "eventFrom"; Layout.preferredWidth: 80; placeholderText: "19:00" }
                Label { text: "–" }
                TextField { id: to; objectName: "eventTo"; Layout.preferredWidth: 80; placeholderText: qsTr("ends") }
                Label { visible: isNaN(part.startsAt); text: qsTr("a day (year-month-day) and a time (hh:mm)"); color: Theme.danger; font.pixelSize: 12 }
            }
            TextField { id: place; objectName: "eventPlace"; Layout.fillWidth: true; placeholderText: qsTr("Where (a #channel, a link, a place)"); maximumLength: 200 }
            TextArea {
                id: description
                objectName: "eventDescription"
                Layout.fillWidth: true
                Layout.minimumHeight: 80
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("More about it (markdown)")
                background: Rectangle { radius: Theme.radius; color: Theme.card; border.width: 1; border.color: Theme.hairline }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Cancel"); onClicked: part.editing = "" }
                PrimaryButton {
                    objectName: "saveEvent"
                    text: part.editing === "new" ? qsTr("Add") : qsTr("Save")
                    enabled: title.text.trim() !== "" && !isNaN(part.startsAt) && !isNaN(part.endsAt)
                    onClicked: part.save()
                }
            }
        }
    }

    Label {
        visible: part.coming.length === 0
        text: qsTr("Nothing planned yet.")
        opacity: 0.6
    }
    Repeater {
        model: part.coming
        delegate: EventCard { required property var modelData; event: modelData; owner: part; Layout.fillWidth: true }
    }
    Label {
        visible: part.past.length > 0
        text: qsTr("This past month")
        font.bold: true
        opacity: 0.7
        Layout.topMargin: 8
    }
    Repeater {
        model: part.past
        delegate: EventCard { required property var modelData; event: modelData; owner: part; Layout.fillWidth: true; opacity: 0.6 }
    }

    component EventCard: Rectangle {
        id: card
        property var event
        property var owner
        objectName: "event_" + (event ? event.title : "")
        implicitHeight: col.implicitHeight + 24
        radius: Theme.radiusLarge
        color: Theme.tint(Theme.text, Theme.hover)
        border.width: 1
        border.color: event && event.me ? Theme.tint(Theme.primary, 0.5) : Theme.hairline
        ColumnLayout {
            id: col
            anchors.fill: parent
            anchors.margins: 12
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: card.event.title; font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; wrapMode: Text.Wrap }
                ToolButton {
                    visible: card.owner.ponds.canManageEvents
                    icon.source: Theme.icon("pencil"); icon.width: 14; icon.height: 14
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Change %1").arg(card.event.title)
                    onClicked: card.owner.startEdit(card.event)
                }
                ToolButton {
                    visible: card.owner.ponds.canManageEvents
                    icon.source: Theme.icon("trash"); icon.width: 14; icon.height: 14; icon.color: Theme.danger
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Delete %1").arg(card.event.title)
                    onClicked: card.owner.ponds.deleteEvent(card.owner.page.id, String(card.event.id))
                }
            }
            Label { text: card.owner.when(card.event); opacity: 0.8; font.pixelSize: 13 }
            Text {
                visible: card.event.place !== ""
                Layout.fillWidth: true
                textFormat: Text.MarkdownText
                text: "📍 " + card.owner.ponds.pageMarkdown(card.event.place)
                color: palette.text
                linkColor: Theme.primary
                wrapMode: Text.Wrap
                onLinkActivated: link => link.startsWith("channel:") ? card.owner.ponds.openChannel(link.substring(8)) : Qt.openUrlExternally(link)
            }
            Text {
                visible: card.event.description !== ""
                Layout.fillWidth: true
                textFormat: Text.MarkdownText
                text: card.owner.ponds.pageMarkdown(card.event.description)
                color: palette.text
                linkColor: Theme.primary
                wrapMode: Text.Wrap
                onLinkActivated: link => link.startsWith("channel:") ? card.owner.ponds.openChannel(link.substring(8)) : Qt.openUrlExternally(link)
            }
            RowLayout {
                spacing: 8
                Label { text: qsTr("%n going", "", card.event.going); opacity: 0.7; font.pixelSize: 12; Layout.fillWidth: true }
                Button {
                    objectName: "going_" + card.event.title
                    text: card.event.me ? qsTr("Not going") : qsTr("Going")
                    highlighted: !card.event.me
                    onClicked: card.owner.ponds.setGoing(card.owner.page.id, String(card.event.id), !card.event.me)
                }
            }
        }
    }
}
