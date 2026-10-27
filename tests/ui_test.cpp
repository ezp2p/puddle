// The app's window, driven like a person would: real mouse and key events on the real QML, against a real Puddle (the
// app's Backend) and a second real Puddle that plays the friend. Offscreen.
#include <QClipboard>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <QQuickStyle>

#include "../src/gui/app/backend.h"
#include "testnode.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QQuickWindow *win;

static void walk(QQuickItem *it, const std::function<bool(QQuickItem *)> &f)
{
    if (!it)
        return;
    if (f(it))
        return;
    for (QQuickItem *c : it->childItems())
        walk(c, f);
}

// every item with this objectName
static QList<QQuickItem *> all(const QString &name, bool visibleOnly = true)
{
    QList<QQuickItem *> out;
    walk(win->contentItem(), [&](QQuickItem *i) {
        if (i->objectName() == name && (!visibleOnly || i->isVisible()))
            out << i;
        return false;
    });
    return out;
}
static QQuickItem *find(const QString &name) { return all(name).value(0); }
// the visible item with this objectName whose text is `text`
static QQuickItem *find_text(const QString &name, const QString &text)
{
    for (QQuickItem *i : all(name))
        if (i->property("text").toString() == text)
            return i;
    return nullptr;
}

// a visible button with this text (for the standard dialog buttons)
static QQuickItem *button(const QString &text)
{
    QQuickItem *hit = nullptr;
    walk(win->contentItem(), [&](QQuickItem *i) {
        if (i->isVisible() && i->metaObject()->inherits(&QQuickItem::staticMetaObject) && i->property("text").toString() == text &&
            QString(i->metaObject()->className()).contains("Button")) {
            hit = i;
            return true;
        }
        return false;
    });
    return hit;
}

static void settle(int ms = 300)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static QJsonObject row(JsonListModel *m, const QString &key, const QString &value)
{
    for (const QJsonObject &o : m->items())
        if (o.value(key).toString() == value)
            return o;
    return {};
}

static QPoint centre(QQuickItem *i) { return i->mapToScene(QPointF(i->width() / 2, i->height() / 2)).toPoint(); }
static bool click(QQuickItem *i)
{
    if (!i || !i->isVisible() || !i->isEnabled())
        return false;
    settle(150); // lists and layouts may still be moving into place
    QTest::mouseClick(win, Qt::LeftButton, {}, centre(i));
    return true;
}
static bool rightClick(QQuickItem *i)
{
    if (!i || !i->isVisible())
        return false;
    settle(150);
    QTest::mouseClick(win, Qt::RightButton, {}, centre(i));
    return true;
}
// turn the wheel over `i`: positive notches scroll up
static void wheel(QQuickItem *i, int notches)
{
    const QPointF p = i->mapToScene(QPointF(i->width() / 2, i->height() / 2));
    QWheelEvent e(p, win->mapToGlobal(p), QPoint(), QPoint(0, notches * 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(win, &e);
}
// the sidebar row of the conversation with this name
static QQuickItem *dm_row(const QString &name)
{
    for (QQuickItem *i : all("dmRow"))
        if (i->property("convName").toString() == name)
            return i;
    return nullptr;
}
// the sidebar row of this kind ("request", "invite", "sent", "dm", "group")
static QQuickItem *row_of_kind(const QString &kind)
{
    for (QQuickItem *i : all("dmRow"))
        if (i->property("kind").toString() == kind)
            return i;
    return nullptr;
}
static bool clickName(const char *name) { return until([&] { return find(name) != nullptr; }, 5000) && click(find(name)); }
static void typeText(const QString &t)
{
    for (const QChar c : t)
        QTest::keyClick(win, c.toLatin1());
}
static void key(Qt::Key k, Qt::KeyboardModifiers m = {}) { QTest::keyClick(win, k, m); }
// the messages as the window draws them
static QStringList shown()
{
    QStringList out;
    for (QQuickItem *i : all("messageText"))
        out << i->property("text").toString();
    return out;
}
static bool shows(const QString &text) { return shown().contains(text); }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("HOME", home.path().toUtf8());
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    app.setOrganizationName("sloporation-ui-test");

    const unsigned port = free_port();
    QDir().mkpath(home.path() + "/.config/puddle");
    QFile cf(home.path() + "/.config/puddle/puddle.yaml");
    CHECK(cf.open(QIODevice::WriteOnly));
    cf.write(QString("address: \"127.0.0.1\"\nport: %1\n").arg(port).toUtf8());
    cf.close();

    Node bob;
    CHECK(bob.up(home.path() + "/friends", "Bob"));
    Chats &B = *bob.chats;
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());

    Backend backend; // the app's own Puddle
    if (backend.needsSetup()) {
        CHECK(!backend.createIdentity().isEmpty());
        CHECK(backend.finishSetup());
    }
    CHECK(backend.running() && backend.error().isEmpty());
    CHECK(backend.myLink() == QString("127.0.0.1:%1").arg(port));
    Chats &A = *backend.chats();
    A.setInterval(100);
    CHECK(until([&] { return A.connected(); }));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appBackend", &backend);
    const QString qml = QString(R"(
        import QtQuick
        import QtQuick.Controls
        import "file://%1/shell" as G
        ApplicationWindow { width: 1000; height: 640; visible: true
            G.Shell { anchors.fill: parent; backend: appBackend } })").arg(PUDDLE_GUI_DIR);
    engine.loadData(qml.toUtf8(), QUrl("file:///ui_test.qml"));
    CHECK(!engine.rootObjects().isEmpty());
    if (engine.rootObjects().isEmpty())
        return 1;
    win = qobject_cast<QQuickWindow *>(engine.rootObjects().at(0));
    win->requestActivate();
    CHECK(until([&] { return win->isActive(); }, 3000));
    settle();

    /* ---- the copy-link button puts our link on the clipboard ---- */
    QGuiApplication::clipboard()->setText("nothing yet");
    CHECK(clickName("copyLinkButton"));
    CHECK(until([&] { return QGuiApplication::clipboard()->text() == backend.myLink(); }, 3000));

    /* ---- ask Bob to be a friend, from the window ---- */
    CHECK(find("homePage") != nullptr);                                                            /* nothing open: the empty page */
    CHECK(clickName("newConversationButton"));
    CHECK(clickName("tabAddFriend"));
    settle();
    if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
        win->grabWindow().save(shot + "-dialog.png");
    CHECK(until([&] { return find("linkField") != nullptr; }, 3000));
    CHECK(click(find("linkField")));
    typeText(bob.link);
    CHECK(find("sendRequestButton") && find("sendRequestButton")->isEnabled());
    CHECK(click(find("noteField")));
    typeText("hi from the window");
    QSignalSpy newReq(&B, &Chats::newRequest);
    CHECK(click(find("sendRequestButton")));
    CHECK(until([&] { return B.incomingRequests() == 1; }));
    CHECK(newReq.count() == 1);
    CHECK(until([&] { return find("linkField") == nullptr; }, 3000)); /* the popup went away */
    CHECK(until([&] { return row_of_kind("sent") != nullptr; }, 5000));                            /* the request is in the list, as "request sent" */
    {
        bool noted = false;
        for (const QJsonObject &o : static_cast<JsonListModel *>(B.people())->items())
            noted = noted || o.value("note").toString() == "hi from the window";
        CHECK(noted);
    }

    /* ---- Bob accepts; the window shows a friend ---- */
    B.accept(backend.myId(), "main");
    CHECK(until([&] { return A.friendCount() == 1; }));
    CHECK(until([&] { return dm_row("Bob") != nullptr && row_of_kind("sent") == nullptr; }, 5000));   /* a friend is in the list before anything is said; the request is no more */

    /* ---- chat: send ---- */
    CHECK(click(dm_row("Bob")));
    CHECK(until([&] { return A.current() == bob.uid; }, 3000));
    CHECK(until([&] { return find("composer") != nullptr; }, 3000));
    /* the panel beside it shows who Bob is, and what can be done; no IDs until Developer Mode */
    CHECK(until([&] { return find("profileName") != nullptr; }, 3000));
    CHECK(find("profileName")->property("text").toString() == "Bob");
    CHECK(find("profileRemove") && find("profileBlock") && find("profileSince") && !find("profileId") && !find("memberList"));
    CHECK(click(find("composer")));
    typeText("hello from the window");
    key(Qt::Key_Return);
    B.open(backend.myId());
    CHECK(until([&] { return msgs_b->rowCount() == 1 && msgs_b->at(0).value("body").toString() == "hello from the window"; }));
    CHECK(until([&] { return shows("hello from the window"); }, 5000));
    CHECK(find("composer")->property("text").toString().isEmpty()); /* the box is empty again */

    /* Shift+Enter is a new line, not a send */
    typeText("one");
    key(Qt::Key_Return, Qt::ShiftModifier);
    typeText("two");
    settle(500);
    CHECK(msgs_b->rowCount() == 1);
    CHECK(find("composer")->property("text").toString() == "one\ntwo");
    key(Qt::Key_A, Qt::ControlModifier);
    key(Qt::Key_Backspace);

    /* ---- chat: receive ---- */
    B.send("hi back");
    CHECK(until([&] { return shows("hi back"); }, 5000));

    /* ---- right-click: a message copies, a text box has the usual edit menu ---- */
    {
        QGuiApplication::clipboard()->setText("nothing yet");
        QQuickItem *text = nullptr;
        for (QQuickItem *i : all("messageText"))
            if (i->property("text").toString() == "hi back")
                text = i;
        CHECK(text && rightClick(text));
        CHECK(until([&] { return find("msgCopy") != nullptr; }, 3000));
        CHECK(find("msgReply") && !find("msgEdit") && !find("msgDelete"));                         /* Bob's message: no edit, no delete */
        CHECK(click(find("msgCopy")));
        CHECK(until([&] { return QGuiApplication::clipboard()->text() == "hi back"; }, 3000));
        CHECK(until([&] { return find("msgCopy") == nullptr; }, 3000));                            /* choosing closes it */

        CHECK(click(find("composer")));
        typeText("abc");
        CHECK(rightClick(find("composer")));
        CHECK(until([&] { return find("menuSelectAll") != nullptr; }, 3000));
        CHECK(find("menuPaste") && find("menuCut") && !find("menuCut")->property("enabled").toBool());  /* nothing selected to cut yet */
        CHECK(click(find("menuSelectAll")));
        CHECK(until([&] { return find("composer")->property("selectedText").toString() == "abc"; }, 3000));
        key(Qt::Key_Backspace);                                                                    /* leave the box empty again */
        CHECK(until([&] { return find("composer")->property("text").toString().isEmpty(); }, 3000));
    }

    /* ---- edit with Up ---- */
    CHECK(click(find("composer")));
    key(Qt::Key_Up);
    CHECK(until([&] { return find("editor") != nullptr; }, 3000));
    key(Qt::Key_A, Qt::ControlModifier);
    typeText("hello edited");
    key(Qt::Key_Return);
    CHECK(until([&] { return msgs_b->rowCount() >= 1 && msgs_b->at(0).value("body").toString() == "hello edited" && msgs_b->at(0).value("edited").toBool(); }));
    CHECK(until([&] { return shows("hello edited") && !shows("hello from the window"); }, 5000));
    CHECK(until([&] { return find("editor") == nullptr; }, 3000));

    /* ---- delete: hover the message, press the bin, confirm ---- */
    {
        QQuickItem *text = nullptr;
        for (QQuickItem *i : all("messageText"))
            if (i->property("text").toString() == "hello edited")
                text = i;
        CHECK(text != nullptr);
        if (text) {
            QTest::mouseMove(win, centre(text));
            CHECK(until([&] { return find("deleteButton") != nullptr; }, 3000));
            CHECK(click(find("deleteButton")));
            CHECK(until([&] { return button("OK") != nullptr; }, 3000));
            CHECK(click(button("OK")));
        }
    }
    CHECK(until([&] { return msgs_b->at(0).value("deleted").toBool(); }));
    CHECK(until([&] { return !shows("hello edited") && shows("Message deleted"); }, 5000));

    /* ---- Bob goes offline: the message says so, and goes through when he is back ---- */
    bob.stop();
    CHECK(click(find("composer")));
    typeText("are you there");
    key(Qt::Key_Return);
    CHECK(until([&] { return shows("are you there"); }, 5000));
    CHECK(until([&] {
        QQuickItem *n = find("deliveryNote");
        if (!n)
            return false;
        bool said = false;
        walk(n, [&](QQuickItem *i) {
            said = said || i->property("text").toString().contains("can't be reached");
            return false;
        });
        return said;
    }, 8000));
    CHECK(find("retryButton") != nullptr);
    CHECK(bob.start());
    CHECK(until([&] { return B.connected(); }, 10000));
    CHECK(click(find("retryButton")));
    CHECK(until([&] { return find("deliveryNote") == nullptr; }, 10000)); /* delivered: nothing more to say */
    CHECK(until([&] {
        for (const QJsonObject &o : msgs_b->items())
            if (o.value("body").toString() == "are you there")
                return true;
        return false;
    }, 10000));

    /* ---- the window out of sight: what arrives stays unread, and is read when it is back ---- */
    win->hide();
    CHECK(until([&] { return !A.attentive(); }, 3000));
    B.send("while you were away");
    CHECK(until([&] { return A.unreadTotal() == 1; }));
    settle(600);
    CHECK(A.unreadTotal() == 1);
    win->show();
    win->requestActivate();
    CHECK(until([&] { return A.attentive(); }, 3000));
    CHECK(until([&] { return A.unreadTotal() == 0; }));
    CHECK(until([&] { return shows("while you were away"); }, 5000));

    /* ---- a narrow window has the conversation to itself; the list slides in over it ---- */
    win->resize(380, 640);
    CHECK(until([&] { return find("chatPage") && !find("sidebar"); }, 3000));
    CHECK(find("rail") == nullptr && find("chatBack") == nullptr);
    CHECK(clickName("sidebarButton"));
    CHECK(until([&] { return find("sidebar") && find("rail") && find("chatPage"); }, 3000));      /* over the conversation, which stays where it is */
    CHECK(find("homeButton") != nullptr);                                                          /* the list keeps the rail (where servers go) beside it */
    CHECK(until([&] { return find("sidebar")->mapToItem(win->contentItem(), QPointF(0, 0)).x() >= 0; }, 3000));   /* (slid fully in) */
    if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty()) {
        settle(400);
        win->grabWindow().save(shot + "-narrow.png");
    }
    CHECK(find("rail")->x() + find("rail")->width() <= find("sidebar")->x() + 1);
    CHECK(find("sidebar")->mapToItem(win->contentItem(), QPointF(find("sidebar")->width(), 0)).x() <= win->width() + 1);
    QTest::keyClick(win, Qt::Key_Escape);                                                          /* the members or profile slide in from the right the same way */
    CHECK(until([&] { return !find("sidebar"); }, 3000));
    CHECK(clickName("contextButton"));
    CHECK(until([&] { return find("contextBar") && find("contextClose") && find("chatPage"); }, 3000));
    if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty()) {
        settle(400);
        win->grabWindow().save(shot + "-narrow-context.png");
    }
    CHECK(clickName("contextClose"));
    CHECK(until([&] { return !find("contextBar"); }, 3000));
    CHECK(clickName("sidebarButton"));
    CHECK(until([&] { return find("sidebar") != nullptr; }, 3000));
    CHECK(clickName("settingsButton"));
    CHECK(until([&] { return find("developerSwitch") && !find("sidebar"); }, 3000));              /* choosing something puts the list away */
    win->resize(1000, 640);
    CHECK(until([&] { return find("sidebar") && find("developerSwitch"); }, 3000));

    /* ---- someone asks us: no IDs or codes on screen, until Developer Mode is on ---- */
    {
        Node carol;
        CHECK(carol.up(home.path() + "/friends", "Carol"));
        QSignalSpy asked(&A, &Chats::newRequest);
        carol.chats->request(backend.myLink(), "carol here", "main"); /* who the window will see her as */
        CHECK(until([&] { return asked.count() == 1 && A.incomingRequests() == 1; }));
        CHECK(until([&] { return find("acceptButton") != nullptr && row_of_kind("request") != nullptr; }, 5000));   /* waiting for us, at the top of the list */
        CHECK(row_of_kind("request")->mapToScene(QPointF(0, 0)).y() < dm_row("Bob")->mapToScene(QPointF(0, 0)).y());                                   /* what waits for an answer is above everyone else */
        CHECK(!backend.developerMode());
        CHECK(rightClick(row_of_kind("request")));
        CHECK(until([&] { return find("convAccept") != nullptr; }, 3000));
        CHECK(find("convDecline") && find("convBlock") && !find("convCopyId"));   /* no ID to copy until Developer Mode is on */
        key(Qt::Key_Escape);
        CHECK(until([&] { return find("convAccept") == nullptr; }, 3000));
        CHECK(clickName("settingsButton"));
        CHECK(until([&] { return find("developerSwitch") != nullptr; }, 3000));
        CHECK(find("myIdField") == nullptr);
        CHECK(click(find("developerSwitch")));                                 /* Developer Mode */
        CHECK(until([&] { return backend.developerMode(); }, 3000));
        CHECK(until([&] { return find("myIdField") != nullptr; }, 3000));
        CHECK(find("myIdField")->property("text").toString() == backend.myId());
        CHECK(rightClick(row_of_kind("request")));
        CHECK(until([&] { return find("convCopyId") != nullptr; }, 3000));
        QGuiApplication::clipboard()->setText("nothing yet");
        CHECK(click(find("convCopyId")));
        CHECK(until([&] { return QGuiApplication::clipboard()->text() == carol.uid; }, 3000));
        backend.setDeveloperMode(false);
        CHECK(rightClick(row_of_kind("request")));
        CHECK(until([&] { return find("convAccept") != nullptr; }, 3000));
        CHECK(!find("convCopyId"));
        key(Qt::Key_Escape);
        CHECK(until([&] { return find("convAccept") == nullptr; }, 3000));
        CHECK(click(find("acceptButton")));                                    /* and it can be accepted without ever seeing an ID */
        CHECK(until([&] { return A.incomingRequests() == 0 && A.friendCount() == 2; }));
    }

    /* ================= groups ================= */
    {
        Node dana;
        CHECK(dana.up(home.path() + "/friends", "Dana"));
        Chats &D = *dana.chats;
        auto *msgs_d = static_cast<JsonListModel *>(D.messages());
        auto *conv_a = static_cast<JsonListModel *>(A.conversations());
        A.request(dana.link, "dana, it is the window");
        CHECK(until([&] { return D.incomingRequests() == 1; }));
        D.accept(backend.myId(), "main");
        CHECK(until([&] { return A.friendCount() == 3; }));
        auto group_rows = [&](JsonListModel *m) { int n = 0; for (const QJsonObject &o : m->items()) n += o.value("kind").toString() == "group"; return n; };

        /* ---- make a group from the window: a name, and which friends ---- */
        CHECK(clickName("newConversationButton"));                                                  /* the "+" beside Direct Messages */
        CHECK(until([&] { return find_text("friendPick", "Bob") && find_text("friendPick", "Dana"); }, 5000));
        CHECK(find("groupNameField") == nullptr);                                                   /* one person is a conversation, not a group: no name asked */
        CHECK(click(find_text("friendPick", "Bob")));
        CHECK(find("groupNameField") == nullptr);
        CHECK(until([&] { return find("pickedChip") != nullptr; }, 3000));                         /* picked people stay in view as chips */
        CHECK(find("pickedChip")->property("text").toString() == "Bob");
        CHECK(click(find("convSearch")));
        typeText("da");                                                                             /* typing finds people by name, any case */
        CHECK(until([&] { return find_text("friendPick", "Dana") != nullptr && find_text("friendPick", "Bob") == nullptr; }, 3000));
        CHECK(find("pickedChip") && find("pickedChip")->property("text").toString() == "Bob");     /* and the pick survives the search */
        CHECK(click(find_text("friendPick", "Dana")));
        CHECK(until([&] { return find("groupNameField") != nullptr; }, 3000));                      /* two or more: a group, which can be named */
        CHECK(click(find("groupNameField")));
        typeText("Weekend");
        CHECK(find("startConversationButton") && find("startConversationButton")->property("text").toString().contains("2"));   /* Bob and Dana */
        QSignalSpy made(&A, &Chats::groupCreated);
        CHECK(click(find("startConversationButton")));
        CHECK(until([&] { return made.count() == 1; }));
        const QString gid = made.at(0).at(0).toString();
        CHECK(until([&] { return A.currentIsGroup() && A.current() == gid && find("composer") != nullptr; }, 5000));   /* the window opens it */
        CHECK(until([&] { return A.currentName() == "Weekend" && A.currentState() == "active"; }, 5000));   /* name and state arrive after the switch */
        CHECK(until([&] { return find("groupNotice") != nullptr; }, 5000));                       /* "You made the group “Weekend”" */
        CHECK(find("groupNotice")->property("text").toString().contains("Weekend") && find("groupNotice")->property("text").toString().contains("You"));

        /* ---- they say yes; bob and dana have never met, and are connected through it ---- */
        CHECK(until([&] { return B.incomingGroupInvites() >= 1 && D.incomingGroupInvites() == 1; }));
        B.acceptGroup(gid, "main");
        D.acceptGroup(gid, "main");
        CHECK(until([&] { return A.members()->property("count").toInt() == 3; }));
        CHECK(until([&] { return row(static_cast<JsonListModel *>(B.people()), "id", dana.uid).value("state").toString() == "scoped"; }));
        CHECK(until([&] {
            int joined = 0;
            for (QQuickItem *i : all("groupNotice"))
                joined += i->property("text").toString().contains("joined");
            return joined == 2;
        }, 8000));

        /* ---- talk ---- */
        CHECK(click(find("composer")));
        typeText("hello group");
        key(Qt::Key_Return);
        CHECK(until([&] { return shows("hello group"); }, 5000));
        D.open(gid, "group");
        CHECK(until([&] {
            for (const QJsonObject &o : msgs_d->items())
                if (o.value("body").toString() == "hello group" && !o.value("senderName").toString().isEmpty() && o.value("senderName").toString() != "You")
                    return true;
            return false;
        }));
        B.open(gid, "group");
        B.send("hi from bob");
        CHECK(until([&] { return shows("hi from bob"); }, 5000));
        CHECK(until([&] {
            for (const QJsonObject &o : msgs_d->items())
                if (o.value("body").toString() == "hi from bob" && o.value("senderName").toString() == "Bob")
                    return true;                                                                   /* dana hears bob directly */
            return false;
        }));
        B.close();
        D.close();

        /* ---- the side panel: the members are beside the conversation (the window is wide), and it can be put away and brought back ---- */
        CHECK(until([&] { return find("contextBar") != nullptr && find("memberList") != nullptr && find("memberList")->property("count").toInt() == 3; }, 5000));
        CHECK(click(find("contextButton")));
        CHECK(until([&] { return find("contextBar") == nullptr; }, 3000));
        CHECK(click(find("contextButton")));
        CHECK(until([&] { return find("contextBar") != nullptr && find("memberList")->property("count").toInt() == 3; }, 3000));

        /* the conversations card folds away the same way, and the rail with it */
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())   /* a picture for looking at, when asked */
            win->grabWindow().save(shot + "-open.png");
        CHECK(find("sidebar") != nullptr && find("rail") != nullptr);
        CHECK(click(find("sidebarButton")));
        CHECK(until([&] { return find("sidebar") == nullptr && find("rail") == nullptr && find("composer") != nullptr; }, 3000));
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
            win->grabWindow().save(shot + "-folded.png");
        CHECK(click(find("sidebarButton")));
        CHECK(until([&] { return find("sidebar") != nullptr && find("rail") != nullptr; }, 3000));

        /* a member: there is a menu for each of the others (not for you) */
        CHECK(until([&] { return all("memberMore").size() == 2; }, 3000));

        /* a new name, from the pencil beside it */
        CHECK(clickName("groupRenameButton"));
        CHECK(until([&] { return find("renameDialogField") != nullptr; }, 3000));
        find("renameDialogField")->forceActiveFocus();
        key(Qt::Key_A, Qt::ControlModifier);
        typeText("Weekend away");
        key(Qt::Key_Return);
        CHECK(until([&] { return A.currentName() == "Weekend away"; }));
        CHECK(until([&] { return row(conv_a, "id", gid).value("name").toString() == "Weekend away"; }));

        /* more people: only friends who are not in it are offered */
        CHECK(clickName("addPeopleButton"));
        CHECK(until([&] { return find_text("friendPick", "Carol") != nullptr; }, 5000));
        CHECK(find_text("friendPick", "Bob") == nullptr && find_text("friendPick", "Dana") == nullptr);
        CHECK(find("tabAddFriend") == nullptr);                                                     /* (this is not the place to add a friend) */
        CHECK(click(find_text("friendPick", "Carol")));
        CHECK(find("startConversationButton") && find("startConversationButton")->property("text").toString().contains("1"));
        CHECK(click(find("startConversationButton")));
        CHECK(until([&] { return find("memberList")->property("count").toInt() == 4; }, 5000));      /* Carol is asked: she shows as invited */

        /* leaving asks first */
        CHECK(clickName("leaveGroupButton"));
        CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
        CHECK(click(find("confirmOk")));
        CHECK(until([&] { return A.current().isEmpty() && group_rows(conv_a) == 0; }, 5000));
        CHECK(until([&] { return find("memberList") == nullptr; }, 3000));                        /* and the panel is gone */

        /* ---- invited: the invitation waits in Pending, and joining says what it costs ---- */
        QSignalSpy invited(&A, &Chats::newGroupInvite);
        B.createGroup("Bob's trip", { backend.myId() }, "main");
        CHECK(until([&] { return A.incomingGroupInvites() == 1 && invited.count() == 1; }));
        CHECK(until([&] { return find("acceptGroupButton") != nullptr && row_of_kind("invite") != nullptr; }, 5000));   /* pinned at the top of the list */
        {
            bool shown = false, counted = false;
            walk(win->contentItem(), [&](QQuickItem *i) {
                const QString t = i->isVisible() ? i->property("text").toString() : QString();
                if (t.contains("invited you")) {
                    shown = true;
                    counted = counted || t.contains(QRegularExpression("\\d")) || t.contains("people") || t.contains("member");   /* it does not say how many are in it */
                }
                return false;
            });
            CHECK(shown && !counted);
        }
        CHECK(click(find("acceptGroupButton")));
        CHECK(until([&] { return button("OK") != nullptr; }, 3000));                               /* "Everyone in this group will be able to see your address" */
        {
            bool said = false;
            walk(win->contentItem(), [&](QQuickItem *i) {
                said = said || (i->isVisible() && i->property("text").toString().contains("see your address"));
                return false;
            });
            CHECK(said);
        }
        CHECK(click(button("OK")));
        CHECK(until([&] { return A.incomingGroupInvites() == 0 && group_rows(conv_a) == 1; }, 5000));
        /* and one that is turned down */
        B.createGroup("Not for me", { backend.myId() }, "main");
        CHECK(until([&] { return A.incomingGroupInvites() == 1; }));
        CHECK(until([&] { return find("declineGroupButton") != nullptr; }, 5000));
        CHECK(click(find("declineGroupButton")));
        CHECK(until([&] { return A.incomingGroupInvites() == 0; }));
        settle(500);
        CHECK(group_rows(conv_a) == 1);

        /* ---- scrolling: the wheel scrolls a list, dragging it with the mouse does not ---- */
        {
            /* (a chat of its own for this, at the end: sixty more lines in the first one would move everything the other checks look for) */
            CHECK(click(dm_row("Bob")));
            CHECK(until([&] { return A.current() == bob.uid && find("messageList") != nullptr; }, 3000));
            B.open(backend.myId());
            for (int i = 0; i < 60; i++)
                B.send(QString("filler %1").arg(i));
            CHECK(until([&] { return shows("filler 59"); }, 15000));
            QQuickItem *list = find("messageList");
            CHECK(list != nullptr);
            auto y = [&] { return list->property("contentY").toReal(); };
            auto atBottom = [&] { return list->property("atBottom").toBool(); };
            CHECK(until([&] { return atBottom() && list->property("contentHeight").toReal() > list->height() + 60; }, 5000));
            {   /* let the list settle (rows being measured, deliveries finishing) before taking where the bottom is */
                qreal last = -1;
                for (int k = 0; k < 20; k++) {
                    settle(250);
                    const qreal now = list->property("contentHeight").toReal();
                    if (now == last && atBottom())
                        break;
                    last = now;
                }
            }
            const qreal bottom = y();

            /* drag the content downwards (towards earlier messages) from the blank gutter beside the pictures: it stays where it is */
            const QPoint from = list->mapToScene(QPointF(8, 60)).toPoint();
            QTest::mousePress(win, Qt::LeftButton, {}, from);
            for (int step = 1; step <= 10; step++)
                QTest::mouseMove(win, from + QPoint(0, step * 30));
            QTest::mouseRelease(win, Qt::LeftButton, {}, from + QPoint(0, 300));
            settle(300);
            CHECK(y() == bottom && atBottom());

            /* the wheel does scroll it, and it stops following new messages until it is back at the end */
            auto first_visible = [&](QString &text, qreal &top_y) {
                const qreal top = list->mapToScene(QPointF(0, 0)).y();
                top_y = 1e9;
                for (QQuickItem *i : all("messageText")) {
                    const qreal yy = i->mapToScene(QPointF(0, 0)).y();
                    if (yy >= top && yy < top_y) {
                        top_y = yy;
                        text = i->property("text").toString();
                    }
                }
                return top_y < 1e9;
            };
            auto *msgs = static_cast<JsonListModel *>(A.messages());
            wheel(list, 1);                                                                            /* one notch is Theme.wheelStep pixels */
            CHECK(until([&] { return y() == bottom - 60; }, 2000));
            CHECK(!list->property("stickToEnd").toBool());
            wheel(list, -10);                                                                          /* and further than the end is the end */
            CHECK(until([&] { return y() == bottom; }, 2000));
            CHECK(list->property("stickToEnd").toBool());

            /* only the newest page is here; scrolling up to within a screenful of the top fetches the one before, which goes above: what you were
             * reading moves only by what you scrolled, not by the page that was added */
            /* (everything that arrived while the chat was open is already held, so open it afresh: then it is the newest page and no more) */
            CHECK(clickName("settingsButton"));
            CHECK(until([&] { return A.current().isEmpty(); }, 3000));
            CHECK(click(find("homeButton")));                                                          /* Home goes back into the conversation that was open */
            CHECK(until([&] { return A.current() == bob.uid; }, 3000));
            CHECK(clickName("settingsButton"));
            CHECK(until([&] { return A.current().isEmpty(); }, 3000));
            CHECK(click(dm_row("Bob")));
            CHECK(until([&] { return A.current() == bob.uid && find("messageList") != nullptr && msgs->rowCount() > 0 && atBottom(); }, 5000));
            settle(500);
            CHECK(A.hasOlder());
            {
                const int rows_first = msgs->rowCount();
                QString anchor;
                qreal anchor_y = 0;
                CHECK(first_visible(anchor, anchor_y));
                int steps = 0;
                while (steps < 20 && msgs->rowCount() == rows_first) {
                    wheel(list, 1);
                    steps++;
                    settle(150);
                }
                CHECK(msgs->rowCount() > rows_first);
                settle(500);
                bool seen = false;
                qreal at = 0;
                for (QQuickItem *i : all("messageText"))
                    if (i->property("text").toString() == anchor) {
                        at = i->mapToScene(QPointF(0, 0)).y();
                        seen = true;
                    }
                CHECK(seen && std::abs(at - (anchor_y + 60 * steps)) < 100);
                wheel(list, -1000);                                                                    /* back to the end for what follows */
                CHECK(until([&] { return atBottom(); }, 3000));
            }
            const qreal bottom2 = y();
            wheel(list, 1000);                                                                         /* up as far as it goes: it moves, and never past the top */
            CHECK(until([&] { return y() < bottom2 - 100; }, 2000));
            CHECK(y() >= list->property("originY").toReal());
            wheel(list, -1000);
            CHECK(until([&] { return atBottom(); }, 3000));

            /* reading back: each time the top is reached the page before arrives, and what you were looking at stays where it was */
            int loads = 0;
            for (int round = 0; round < 12 && A.hasOlder(); round++) {
                const int rows = msgs->rowCount();
                wheel(list, 1000);
                QString was;
                qreal was_y = 0;
                CHECK(until([&] { return first_visible(was, was_y); }, 2000));
                CHECK(until([&] { return msgs->rowCount() > rows || !A.hasOlder(); }, 5000));           /* the earlier page came (or there was no more) */
                settle(400);
                if (msgs->rowCount() == rows)
                    continue;
                loads++;
                qreal now_y = 0;
                bool same = false;
                for (QQuickItem *i : all("messageText"))
                    if (i->property("text").toString() == was) {
                        now_y = i->mapToScene(QPointF(0, 0)).y();
                        same = true;
                    }
                CHECK(same && std::abs(now_y - was_y) < 80);                                          /* what you were reading did not jump */
            }
            (void)loads;
            CHECK(!A.hasOlder());                                                                       /* all the way back */
            CHECK(msgs->at(0).value("kind").toString() == "message" && msgs->at(0).value("deleted").toBool());   /* the first thing said (later taken back) is there */
        }
        /* ---- right-click on a conversation: the group's menu, and the friend's ---- */
        {
            QQuickItem *g = dm_row("Bob's trip");
            CHECK(g && rightClick(g));
            CHECK(until([&] { return find("convOpen") != nullptr; }, 3000));
            CHECK(find("convRename") && find("convLeave") && !find("convRemove") && !find("convBlock"));
            CHECK(click(find("convRename")));
            CHECK(until([&] { return find("renameDialogField") != nullptr; }, 3000));
            find("renameDialogField")->forceActiveFocus();
            key(Qt::Key_A, Qt::ControlModifier);
            typeText("Bob trip two");
            key(Qt::Key_Return);
            CHECK(until([&] { return group_rows(conv_a) == 1 && dm_row("Bob trip two") != nullptr; }, 5000));

            /* leaving asks first: Cancel keeps the group, the red button leaves it */
            g = dm_row("Bob trip two");
            CHECK(g && rightClick(g));
            CHECK(clickName("convLeave"));
            CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
            CHECK(click(find("confirmCancel")));
            CHECK(until([&] { return find("confirmOk") == nullptr; }, 3000));
            settle(300);
            CHECK(group_rows(conv_a) == 1);
            CHECK(rightClick(dm_row("Bob trip two")));
            CHECK(clickName("convLeave"));
            CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
            CHECK(click(find("confirmOk")));
            CHECK(until([&] { return group_rows(conv_a) == 0; }, 5000));

            /* a friend: removing asks first too */
            const int friends = A.friendCount();
            QQuickItem *f = dm_row("Bob");
            CHECK(f && rightClick(f));
            CHECK(until([&] { return find("convRemove") != nullptr; }, 3000));
            CHECK(find("convBlock") && !find("convRename") && !find("convLeave"));
            CHECK(click(find("convRemove")));
            CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
            CHECK(click(find("confirmCancel")));
            settle(300);
            CHECK(A.friendCount() == friends);
            CHECK(rightClick(dm_row("Bob")));
            CHECK(clickName("convRemove"));
            CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
            CHECK(click(find("confirmOk")));
            CHECK(until([&] { return A.friendCount() == friends - 1; }, 5000));
        }
    }

    return failures ? 1 : 0;
}
