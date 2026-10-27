import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// The "+" of the sidebar. Message: pick one friend to talk to, or several to start a group. Add friend: ask someone new, with their address.
Popup {
    id: pop

    required property var chats
    required property var backend
    signal startDm(string userId)   // one friend was picked; a group is made through chats.createGroup and announced by groupCreated
    // set while the dialog is asking more friends into a group that exists (instead of starting something new)
    property string addTo: ""
    property string addToName: ""
    readonly property bool adding: addTo !== ""

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 24 : 460)
    // never taller than the window: the list of friends is what gives way
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 600) - 24)
    padding: 20
    background: Rectangle { color: Theme.card; radius: Theme.cardRadius - 6; border.width: 1; border.color: Theme.hairline }
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property int maxPicked: 49   // a group has room for 50 people, and you are one
    readonly property int shownMax: 30
    // who the search finds: the most recent friends until you type, then those whose name has what you typed (never more than a screenful: type more)
    // (when asking more into a group, those already in it are not offered)
    function candidates(limit) {
        const found = chats.findFriends(search.text, adding ? 100000 : limit)
        return adding ? found.filter(f => !chats.isGroupMember(f.id)).slice(0, limit) : found
    }
    readonly property var results: { chats.friendCount; chats.sidebar.count; chats.members.count; return candidates(shownMax) }
    readonly property int matches: { chats.friendCount; chats.members.count; return adding ? candidates(100000).length : chats.friendMatches(search.text.trim()) }
    property int tab: 0   // 0 message, 1 add friend
    property var picked: ({})
    property int pickedCount: 0

    function show(which) {
        addTo = ""
        tab = which
        open()
    }
    // ask friends who are not in the open group yet
    function showAdd(groupId, groupName) {
        addTo = groupId
        addToName = groupName
        tab = 0
        open()
    }
    onOpened: (tab === 0 ? search : link).forceActiveFocus()
    onClosed: {
        search.text = ""; groupName.text = ""; link.text = ""; note.text = ""
        picked = ({}); pickedCount = 0
    }

    function pickedIds() { return Object.keys(picked).filter(k => picked[k]) }
    function pick(id) { const p = picked; p[id] = true; picked = p; pickedCount = pickedIds().length }
    function unpick(id) { const p = picked; delete p[id]; picked = p; pickedCount = pickedIds().length }
    // a name for a group nobody has named: the first few people in it
    function defaultName() {
        const names = pickedIds().map(id => chats.nameOf(id))
        const shown = names.slice(0, 3).join(", ")
        return names.length > 3 ? shown + " +" + (names.length - 3) : shown
    }
    function start() {
        const ids = pickedIds()
        if (ids.length === 0) return
        if (adding) {
            for (const id of ids) chats.inviteToGroup(id)
        } else if (ids.length === 1) {
            startDm(ids[0])
        } else {
            const name = groupName.text.trim() !== "" ? groupName.text.trim() : defaultName()
            chats.createGroup(name.slice(0, 100), ids)
        }
        close()
    }
    function send() {
        const t = link.text.trim()
        if (t === "") return
        chats.request(t, note.text.trim(), profile.count > 0 ? profile.currentValue : "") // who they will see us as: our consent to show it them
        close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label { text: pop.adding ? qsTr("Add people to “%1”").arg(pop.addToName) : qsTr("New conversation"); font.bold: true; font.pixelSize: 20; elide: Text.ElideRight; Layout.fillWidth: true }

        RowLayout {
            visible: !pop.adding
            spacing: 4
            Repeater {
                model: [qsTr("Message"), qsTr("Add friend")]
                delegate: AbstractButton {
                    id: tb
                    objectName: index === 0 ? "tabMessage" : "tabAddFriend"
                    required property int index
                    required property string modelData
                    readonly property bool current: pop.tab === index
                    text: modelData
                    implicitWidth: tbLabel.implicitWidth + 24
                    implicitHeight: 30
                    contentItem: Label { id: tbLabel; text: tb.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: tb.current }
                    Accessible.name: text
                    onClicked: pop.tab = index
                    background: Rectangle {
                        radius: Theme.buttonRadius
                        color: tb.current ? Theme.tint(palette.text, 0.12) : tb.hovered ? Theme.tint(palette.text, 0.06) : "transparent"
                    }
                }
            }
        }

        // ---- message ----
        ColumnLayout {
            visible: pop.tab === 0 || pop.adding
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            TextField {
                id: search
                objectName: "convSearch"
                Layout.fillWidth: true
                leftPadding: 34
                placeholderText: qsTr("Type a name")
                selectByMouse: true
                Accessible.name: qsTr("Search friends")
                FieldMenu {}
                Icon { glyph: "search"; size: 16; opacity: 0.5; x: 10; anchors.verticalCenter: parent.verticalCenter }
            }

            // who you have picked, so it stays in view whatever you search for next
            Flow {
                id: chips
                objectName: "pickedChips"
                visible: pop.pickedCount > 0
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: { pop.pickedCount; return pop.pickedIds() }
                    delegate: Button {
                        id: chip
                        required property string modelData
                        objectName: "pickedChip"
                        text: pop.chats.nameOf(modelData)
                        icon.source: Theme.icon("x"); icon.width: 14; icon.height: 14
                        implicitHeight: 32
                        LayoutMirroring.enabled: true
                        LayoutMirroring.childrenInherit: false
                        Accessible.name: qsTr("Remove %1").arg(text)
                        onClicked: pop.unpick(modelData)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                font.pixelSize: 11
                font.bold: true
                opacity: 0.6
                elide: Text.ElideRight
                visible: pop.chats.friendCount > 0
                text: search.text.trim() === "" ? qsTr("Recent")
                      : pop.matches === 0 ? qsTr("No friend has “%1” in their name").arg(search.text.trim())
                      : pop.matches > pop.results.length ? qsTr("Showing %1 of %2. Keep typing to narrow it.").arg(pop.results.length).arg(pop.matches)
                      : pop.matches === 1 ? qsTr("1 friend") : qsTr("%1 friends").arg(pop.matches)
            }

            ScrollList {
                id: friends
                objectName: "friendList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 64
                Layout.preferredHeight: Math.min(260, Math.max(80, pop.results.length * 44))
                model: pop.results
                delegate: CheckDelegate {
                    id: pick
                    required property var modelData
                    objectName: "friendPick"
                    width: ListView.view.width
                    height: 44
                    text: modelData.name
                    checked: !!pop.picked[modelData.id]
                    enabled: checked || pop.pickedCount < pop.maxPicked
                    onToggled: checked ? pop.pick(modelData.id) : pop.unpick(modelData.id)
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: pop.chats.friendCount === 0
                    opacity: 0.6
                    text: qsTr("You have no friends to talk to yet. Use “Add friend” to ask someone.")
                }
            }

            // more than one person is a group
            ColumnLayout {
                visible: pop.pickedCount > 1 && !pop.adding
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: groupName
                    objectName: "groupNameField"
                    Layout.fillWidth: true
                    placeholderText: { pop.pickedCount; return pop.defaultName() }   // (the count changes when the names do)
                    selectByMouse: true
                    onTextChanged: if (length > 100) remove(100, length)
                    Accessible.name: qsTr("Group name")
                    onAccepted: pop.start()
                    FieldMenu {}
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: pop.pickedCount >= pop.maxPicked
                          ? qsTr("A group has room for 50 people, so that is as many friends as you can ask now.")
                          : qsTr("Everyone in a group can see each other's address and connect directly, even people who are not friends. You can ask more people later.")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Cancel"); onClicked: pop.close() }
                PrimaryButton {
                    id: startButton
                    objectName: "startConversationButton"
                    text: pop.adding ? (pop.pickedCount === 1 ? qsTr("Ask 1 friend") : qsTr("Ask %1 friends").arg(pop.pickedCount))
                          : pop.pickedCount > 1 ? qsTr("Create group with %1 people").arg(pop.pickedCount) : qsTr("Message")
                    enabled: pop.pickedCount > 0
                    onClicked: pop.start()
                }
            }
        }

        // ---- add friend ----
        ColumnLayout {
            visible: pop.tab === 1 && !pop.adding
            Layout.fillWidth: true
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("Enter their address, like 203.0.113.7 or home.example.org:8443. They will see your request and decide whether to accept.")
            }
            TextField {
                id: link
                objectName: "linkField"
                Layout.fillWidth: true
                placeholderText: qsTr("Address, e.g. home.example.org")
                selectByMouse: true
                Accessible.name: qsTr("Their address")
                onAccepted: pop.send()
                FieldMenu {}
            }
            TextField {
                id: note
                objectName: "noteField"
                Layout.fillWidth: true
                placeholderText: qsTr("Say who you are (optional)")
                maximumLength: 500
                selectByMouse: true
                Accessible.name: qsTr("Note")
                onAccepted: pop.send()
                FieldMenu {}
            }
            RowLayout {
                visible: profile.count > 0 // always said: it is what they will see of us
                Layout.fillWidth: true
                Label { text: qsTr("Appear as") }
                ComboBox {
                    id: profile
                    Layout.fillWidth: true
                    model: pop.chats.profiles
                    textRole: "display_name"
                    valueRole: "id"
                    Accessible.name: qsTr("Profile to show")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Cancel"); onClicked: pop.close() }
                PrimaryButton {
                    id: sendButton
                    objectName: "sendRequestButton"
                    text: qsTr("Send Request")
                    enabled: link.text.trim() !== ""
                    onClicked: pop.send()
                }
            }

            MenuSeparator { Layout.fillWidth: true }

            Label { text: qsTr("Your address"); font.bold: true }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                visible: pop.backend.myLink === ""
                text: qsTr("Puddle does not know which address reaches you from outside. Set “advertise:” in puddle.yaml, then restart.")
            }
            RowLayout {
                visible: pop.backend.myLink !== ""
                Layout.fillWidth: true
                TextField {
                    Layout.fillWidth: true
                    readOnly: true
                    text: pop.backend.myLink
                    selectByMouse: true
                    Accessible.name: qsTr("Your address")
                    FieldMenu {}
                }
                Button { text: qsTr("Copy"); onClicked: pop.backend.copyText(pop.backend.myLink) }
            }
        }
    }
}
