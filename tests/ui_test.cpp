// The app's window, driven like a person would: real mouse and key events on the real QML, against a real Puddle (the
// app's Backend) and a second real Puddle that plays the friend. Offscreen.
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QMimeData>
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
#include "../src/gui/app/composer.h"
#include "../src/gui/app/phone.h"
#include "testnode.h"
#include "federation/calls.h"
extern "C" {
#include "core/backup.h"
#include "core/devices.h"
}
#include <QSettings>

#include <QPainter>
#include <algorithm>
#include <atomic>

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
// Settings scrolls: bring `i` into the window first if it is on that page, as the owner would scroll to it
static void reveal(QQuickItem *i)
{
    for (QQuickItem *p = i ? i->parentItem() : nullptr; p; p = p->parentItem())
        if (p->objectName() == "settingsScroll") {
            QQuickItem *content = p->property("contentItem").value<QQuickItem *>();
            if (!content)
                return;
            const double y = i->mapToItem(content, QPointF(0, 0)).y(), view = p->height();
            const double cy = p->property("contentY").toDouble();
            if (y < cy || y + i->height() > cy + view) {
                const double most = std::max(0.0, p->property("contentHeight").toDouble() - view);
                p->setProperty("contentY", std::clamp(y - view / 3, 0.0, most));
                settle(100);
            }
            return;
        }
}
static bool click(QQuickItem *i)
{
    if (!i || !i->isVisible() || !i->isEnabled())
        return false;
    reveal(i);
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
    /* Playing a video needs the system's own event loop (on macOS, AVFoundation answers through it), so a run with
     * PUDDLE_TEST_VIDEO=<a short clip> shows its window; the checks of the window having focus then depend on the desktop. */
    if (qgetenv("PUDDLE_TEST_VIDEO").isEmpty())
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("HOME", home.path().toUtf8());
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    app.setOrganizationName("ezp2p-ui-test");

    const unsigned port = free_port();
    QDir().mkpath(home.path() + "/.config/puddle");
    QFile cf(home.path() + "/.config/puddle/puddle.yaml");
    CHECK(cf.open(QIODevice::WriteOnly));
    cf.write(QString("address: \"127.0.0.1\"\nport: %1\nfiles:\n  auto_download: 1 KiB\n").arg(port).toUtf8()); /* files over 1 KiB wait to be asked for */
    cf.close();

    Node bob;
    CHECK(bob.up(home.path() + "/friends", "Bob"));
    Chats &B = *bob.chats;
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());

    qputenv("PUDDLE_TEST_NO_AUDIO", "1"); // calls below: not this computer's microphone and speaker
    {   /* the first run asks where the Puddle is; "remote", then back, leaves nothing behind */
        Backend first;
        CHECK(first.mode().isEmpty() && !first.running() && !first.needsSetup() && !first.phone());
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty()) { /* how the first run looks */
            QQmlApplicationEngine e;
            e.rootContext()->setContextProperty("firstBackend", &first);
            e.loadData(QString(R"(
                import QtQuick
                import QtQuick.Controls
                import "file://%1/setup" as S
                ApplicationWindow { width: 900; height: 600; visible: true
                    S.Welcome { anchors.fill: parent; backend: firstBackend; visible: !rec.visible }
                    S.Setup { id: rec; objectName: "rec"; anchors.fill: parent; backend: firstBackend; recovering: true; visible: false } })")
                           .arg(PUDDLE_GUI_DIR).toUtf8(), QUrl("file:///welcome.qml"));
            if (auto *w = qobject_cast<QQuickWindow *>(e.rootObjects().value(0))) {
                QTest::qWait(300);
                w->grabWindow().save(shot + "-welcome.png");
                if (QObject *rec = w->findChild<QObject *>("rec")) {
                    rec->setProperty("visible", true);
                    QTest::qWait(200);
                    w->grabWindow().save(shot + "-recover.png");
                }
            }
        }
        CHECK(first.choose("remote") && first.mode() == "remote" && first.phone() && !first.running());
        CHECK(!first.choose("here")); /* chosen already */
        CHECK(first.unchoose() && first.mode().isEmpty() && !first.phone());
        CHECK(first.choose("here") && first.needsSetup() && !first.hasLocalPuddle());
        CHECK(first.unchoose() && first.mode().isEmpty());
    }
    {   /* asked again on the next run, since nothing was made */
        Backend again;
        CHECK(again.mode().isEmpty());
        CHECK(again.choose("here"));
    }
    Backend backend; // the app's own Puddle: chosen above, so remembered
    CHECK(backend.mode() == "here");
    QString phrase;
    if (backend.needsSetup()) {
        phrase = backend.createIdentity();
        CHECK(!phrase.isEmpty());
        CHECK(backend.finishSetup());
    }
    CHECK(backend.running() && backend.error().isEmpty() && backend.hasLocalPuddle());
    CHECK(!backend.unchoose()); /* a Puddle runs here: only Settings, with a restart, asks again */
    CHECK(backend.myLink().startsWith(QString("127.0.0.1:%1?k=").arg(port)) && backend.myLink().size() == QString("127.0.0.1:%1?k=").arg(port).size() + 52);
    Chats &A = *backend.chats();
    A.setInterval(100);
    CHECK(until([&] { return A.connected(); }));

    qmlRegisterType<Composer>("Puddle.Files", 1, 0, "Composer");
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

    /* ---- a call (docs/calls.md): from the window to Bob, who answers; mute; hang up; the chat notes it ---- */
    {
        Calls &ca = *A.callModel(), &cb = *bob.chats->callModel();
        auto note = [&](const QString &start) {
            for (QQuickItem *i : all("groupNotice"))
                if (i->property("text").toString().startsWith(start))
                    return true;
            return false;
        };
        CHECK(until([&] { return find("callButton") && find("callButton")->property("enabled").toBool(); }, 5000));
        CHECK(click(find("callButton")));
        CHECK(until([&] { return cb.state() == "ringing"; }, 10000));
        CHECK(until([&] { QQuickItem *s = find("voicePanelStatus"); return s && s->property("text").toString() == "Ringing…"; }, 3000));
        CHECK(!find("callButton")->property("enabled").toBool()); /* one call at a time */
        cb.accept();
        CHECK(until([&] { return ca.state() == "active"; }, 15000));
        CHECK(until([&] { QQuickItem *s = find("voicePanelStatus"); return s && s->property("text").toString().startsWith("In a call · 0:0"); }, 5000));
        CHECK(click(find("voicePanelMute")));
        CHECK(until([&] { return ca.muted(); }, 2000));

        /* ---- video (docs/calls.md, "Video") ---- */
        if (CallVideo::isBuilt()) {
            auto *video = static_cast<CallVideo *>(backend.video());
            auto picture = [](int k) {
                QImage img(640, 480, QImage::Format_RGB32);
                img.fill(qRgb(40, 90, 160));
                QPainter p(&img);
                p.fillRect(50 + k * 12, 150, 100, 100, Qt::yellow);
                return img;
            };
            /* the camera, from the bar: the window's own picture shows, and what it takes reaches Bob's Puddle */
            static std::atomic<int> atBob{ 0 };
            puddle_calls_on_video(puddle_server_calls(bob.srv), [](void *, const char *, const char *, const unsigned char *, size_t, uint32_t) { atBob++; }, nullptr);
            CHECK(until([&] { return find("voicePanelCamera") && find("voicePanelCamera")->isVisible(); }, 3000));
            CHECK(click(find("voicePanelCamera")));
            CHECK(until([&] { return ca.camera() && video->cameraOn(); }, 2000));
            /* the call's chat is open: its video is in the chat's pane, above the messages (not the floating tiles) */
            CHECK(until([&] { return find("videoTile_self") && find("videoPane") && find("callVideo") == nullptr; }, 3000));
            for (int k = 0; k < 20 && atBob < 5; k++) { /* there is no camera here: pictures stand in for it */
                video->feed(picture(k));
                until([] { return false; }, 66);
            }
            CHECK(atBob >= 5);
            puddle_calls_on_video(puddle_server_calls(bob.srv), nullptr, nullptr);
            /* Bob's camera: a tile for him in the window */
            H264Encoder enc;
            CHECK(enc.open(CallVideo::kWidth, CallVideo::kHeight, CallVideo::kFps, CallVideo::kBitrate));
            const QByteArray bid = cb.callId().toUtf8();
            for (int k = 0; k < 20; k++) {
                const QByteArray au = enc.encode(pictureOf(picture(k), CallVideo::kWidth, CallVideo::kHeight), k * 66);
                if (!au.isEmpty())
                    puddle_call_send_video(puddle_server_calls(bob.srv), bid.constData(), reinterpret_cast<const unsigned char *>(au.constData()), size_t(au.size()));
                until([] { return false; }, 66);
            }
            CHECK(until([&] { return video->sources() == QStringList{ bob.uid }; }, 3000));
            /* a tile, named: any label under it that says `text` */
            std::function<bool(QQuickItem *, const QString &)> says = [&](QQuickItem *it, const QString &text) {
                if (!it)
                    return false;
                if (it->property("text").toString() == text)
                    return true;
                for (QQuickItem *c : it->childItems())
                    if (says(c, text))
                        return true;
                return false;
            };
            CHECK(until([&] { return says(find("videoTile_" + bob.uid), "Bob"); }, 3000));

            /* our screen, shared from the bar: its own tile here, and it reaches Bob's Puddle as ours "/screen" */
            static std::atomic<int> screenAtBob{ 0 };
            puddle_calls_on_video(puddle_server_calls(bob.srv), [](void *, const char *, const char *from, const unsigned char *, size_t, uint32_t) {
                if (QString::fromUtf8(from).endsWith("/screen"))
                    screenAtBob++;
            }, nullptr);
            CHECK(until([&] { return find("voicePanelScreen") && find("voicePanelScreen")->isVisible(); }, 3000));
            CHECK(click(find("voicePanelScreen")));
            CHECK(until([&] { return ca.screen() && video->screenOn() && find("videoTile_self/screen"); }, 2000));
            for (int k = 0; k < 20 && screenAtBob < 3; k++) {
                QImage desk(1600, 900, QImage::Format_RGB32);
                desk.fill(qRgb(230, 230, 230));
                video->feedScreen(desk);
                until([] { return false; }, 110);
            }
            CHECK(screenAtBob >= 3);
            puddle_calls_on_video(puddle_server_calls(bob.srv), nullptr, nullptr);
            CHECK(click(find("voicePanelScreen")));
            CHECK(until([&] { return !ca.screen() && !video->screenOn() && find("videoTile_self/screen") == nullptr; }, 2000));

            /* Bob shares his: a tile of its own, "Bob's screen"; clicking it pins it large, the rest in a strip */
            H264Encoder senc;
            CHECK(senc.open(CallVideo::kScreenWidth, CallVideo::kScreenHeight, CallVideo::kScreenFps, CallVideo::kScreenBitrate));
            auto bobShares = [&](int frames) {
                for (int k = 0; k < frames; k++) {
                    QImage desk(1280, 720, QImage::Format_RGB32);
                    desk.fill(qRgb(250, 250, 250));
                    const QByteArray au = senc.encode(pictureFitted(desk, CallVideo::kScreenWidth, CallVideo::kScreenHeight), k * 100);
                    if (!au.isEmpty())
                        puddle_call_send_screen(puddle_server_calls(bob.srv), bid.constData(), reinterpret_cast<const unsigned char *>(au.constData()), size_t(au.size()));
                    const QByteArray cam = enc.encode(pictureOf(picture(k), CallVideo::kWidth, CallVideo::kHeight), 2000 + k * 100);
                    if (!cam.isEmpty())
                        puddle_call_send_video(puddle_server_calls(bob.srv), bid.constData(), reinterpret_cast<const unsigned char *>(cam.constData()), size_t(cam.size()));
                    until([] { return false; }, 100);
                }
            };
            bobShares(10);
            const QString bobScreen = bob.uid + "/screen";
            CHECK(until([&] { return video->sources() == (QStringList{ bob.uid, bobScreen }); }, 3000)); /* in the order they came */
            CHECK(until([&] { return says(find("videoTile_" + bobScreen), "Bob's screen"); }, 3000));
            QObject *view = win->findChild<QObject *>("videoView");
            CHECK(view && click(find("videoTile_" + bobScreen)));
            CHECK(view && view->property("pinned").toString() == bobScreen);
            /* full size: the messages make way; pop out: a window of its own, and none in the page */
            CHECK(click(find("videoFull")));
            CHECK(until([&] { return find("messageList") == nullptr; }, 2000));
            CHECK(click(find("videoFull")));
            CHECK(until([&] { return find("messageList") != nullptr; }, 2000));
            CHECK(click(find("videoPopOut")));
            CHECK(until([&] { return view->property("popped").toBool() && find("videoPane") == nullptr; }, 2000));
            QQuickWindow *popped = nullptr;
            for (QWindow *w : QGuiApplication::topLevelWindows())
                if (w->objectName() == "videoWindow" || w->title() == "Puddle: video")
                    popped = qobject_cast<QQuickWindow *>(w);
            CHECK(popped && popped->isVisible());
            view->setProperty("popped", false); /* put back */
            view->setProperty("pinned", QString());
            bobShares(5);
            CHECK(until([&] { return find("videoPane") != nullptr && !(popped && popped->isVisible()); }, 3000));

            /* the camera off: everyone keeps a tile for the whole call, with their picture while no video comes; Bob's
             * screen tile goes once his pictures stop */
            CHECK(click(find("voicePanelCamera")));
            CHECK(until([&] { return !ca.camera() && !video->cameraOn() && find("videoTile_self") && !find("videoTile_self")->property("live").toBool(); }, 2000));
            CHECK(until([&] { return find("videoTile_" + bobScreen) == nullptr && find("videoTile_" + bob.uid) &&
                                     !find("videoTile_" + bob.uid)->property("live").toBool(); }, 6000));
            CHECK(find("videoPane") != nullptr && find("videoSplit") != nullptr); /* the chat, split: video above, messages below */

            /* elsewhere, the call's name in the voice panel above your row takes you back to its chat */
            CHECK(clickName("settingsButton"));
            CHECK(until([&] { return find("videoPane") == nullptr; }, 2000));
            CHECK(find("voicePanel") != nullptr && find("callBar") == nullptr); /* the floating bar is only for ringing, out of sight */
            CHECK(click(find("voicePanelName")));
            CHECK(until([&] { return A.current() == bob.uid && find("videoPane") != nullptr; }, 3000));
            CHECK(ca.muted()); /* (muted earlier) */
            CHECK(click(find("voicePanelMute")));
            CHECK(until([&] { return !ca.muted() && !find("voicePanelMute")->property("highlighted").toBool(); }, 2000));
            CHECK(click(find("voicePanelMute")));
            CHECK(until([&] { return ca.muted(); }, 2000));
        }
        CHECK(clickName("voicePanelLeave"));
        CHECK(until([&] { return ca.state().isEmpty() && cb.state().isEmpty(); }, 10000));
        CHECK(until([&] { return find("voicePanel") == nullptr && note("You called Bob · 0:0"); }, 5000)); /* slid away */
        CHECK(!ca.muted()); /* the next call starts unmuted */

        /* Bob calls the window: the bar rings; declined */
        cb.call(backend.myId());
        CHECK(until([&] { return find("voicePanelAnswer") != nullptr; }, 10000));
        CHECK(find("callBar") == nullptr); /* the list is in sight: ringing is in its panel */
        CHECK(click(find("voicePanelDecline")));
        CHECK(until([&] { return ca.state().isEmpty() && cb.state().isEmpty(); }, 10000));
        CHECK(until([&] { return note("You declined a call from Bob"); }, 5000));
        /* and again, with the left-hand list folded away: a bar floats over the page instead, answered there, and Bob hangs up */
        QQuickItem *shellItem = nullptr;
        walk(win->contentItem(), [&](QQuickItem *i) { if (!shellItem && i->property("showSidebar").isValid()) shellItem = i; return false; });
        CHECK(shellItem != nullptr);
        shellItem->setProperty("showSidebar", false);
        cb.call(backend.myId());
        CHECK(until([&] { return find("acceptCallButton") != nullptr && find("voicePanelAnswer") == nullptr; }, 10000));
        CHECK(click(find("acceptCallButton")));
        CHECK(until([&] { return find("callBar") == nullptr; }, 5000)); /* answered: only ringing floats */
        shellItem->setProperty("showSidebar", true);
        CHECK(until([&] { return ca.state() == "active" && cb.state() == "active"; }, 15000));
        cb.hangUp();
        CHECK(until([&] { return ca.state().isEmpty(); }, 10000));
        CHECK(until([&] { return note("Bob called you · 0:0"); }, 5000));
    }

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
            const QString t = i->property("text").toString();
            said = said || (t.contains("Not delivered to") && t.contains("cannot reach") && t.contains("Trying again at")); /* who, why, and when next */
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
        CHECK(until([&] { return find("acceptOk") != nullptr && find("appearAs") != nullptr; }, 3000));   /* first: which profile she will see */
        CHECK(find("appearAs")->property("currentValue").toString() != "");
        settle(300);
        CHECK(A.incomingRequests() == 1);                                      /* nothing is accepted until it is said */
        CHECK(click(find("acceptOk")));
        CHECK(until([&] { return A.incomingRequests() == 0 && A.friendCount() == 2; }));
        CHECK(until([&] { return carol.chats->nameOf(backend.myId()) == A.myName(); }, 10000));   /* she sees the profile chosen, not an id */
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

        /* ---- someone's card: a click on their name in the chat opens it under the name, with their profile in this group;
         * a right-click opens what can be done about them (a friend: Profile and Message, nothing a Pond's admin does) ---- */
        {
            QQuickItem *bobName = nullptr;
            CHECK(until([&] {
                for (QQuickItem *i : all("messageSenderName"))
                    if (i->property("text").toString() == "Bob")
                        bobName = i;
                return bobName != nullptr;
            }, 5000));
            CHECK(bobName && click(bobName));
            CHECK(until([&] { QQuickItem *n = find("profileCardName"); return n && n->property("text").toString() == "Bob"; }, 5000));
            QQuickItem *card = find("profileCard");
            {   /* under the name, or over it where there is no room below (the newest messages are at the foot of the window) */
                QQuickItem *band = find("profileCardBand"); /* the card's top; its content is the card, less its padding (none) */
                card = band;
                const double top = band ? band->mapToScene(QPointF(0, 0)).y() : 0, bottom = band ? top + band->parentItem()->height() : 0;
                const double nameTop = bobName->mapToScene(QPointF(0, 0)).y(), nameBottom = nameTop + bobName->height();
                CHECK(card && (top >= nameBottom || bottom <= nameTop));
            }
            CHECK(find("profileCardMessage") != nullptr); /* a friend: Message */
            settle(300);
            if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
                win->grabWindow().save(shot + "-chat-card.png");
            key(Qt::Key_Escape);
            CHECK(until([&] { return find("profileCardName") == nullptr; }, 3000));
            CHECK(rightClick(bobName));
            CHECK(until([&] { return find("personProfile") != nullptr; }, 3000));
            CHECK(find("personMessage") && !find("personKick") && !find("personMute") && !find("personBan"));
            CHECK(click(find("personProfile")));
            CHECK(until([&] { QQuickItem *n = find("profileCardName"); return n && n->property("text").toString() == "Bob"; }, 5000));
            key(Qt::Key_Escape);
            settle(200);
        }

        /* ---- a call in the group (docs/calls.md, "Group calls"): started from the header, Bob and Dana join; the bar says how
         * many are in it; leaving leaves it to them, and the header offers to join again; the chat notes it ---- */
        {
            Calls &ca = *A.callModel(), &cb = *B.callModel(), &cd = *D.callModel();
            auto status = [&] { QQuickItem *s = find("voicePanelStatus"); return s ? s->property("text").toString() : QString(); };
            CHECK(until([&] { return find("callButton") && find("callButton")->property("enabled").toBool(); }, 5000));
            CHECK(click(find("callButton")));
            CHECK(until([&] { return cb.state() == "ringing" && cd.state() == "ringing" && cb.group() == gid && cb.name() == "Weekend"; }, 10000));
            CHECK(until([&] { return status() == "Waiting for others…"; }, 3000));
            cb.accept();
            cd.callGroup(gid);
            CHECK(until([&] { return ca.state() == "active" && ca.others() == 2; }, 15000));
            CHECK(until([&] { return status().endsWith("2 others"); }, 5000));
            CHECK(until([&] { return cb.others() == 2 && cd.others() == 2 && cd.state() == "active"; }, 15000));
            CHECK(clickName("voicePanelLeave"));
            CHECK(until([&] { return ca.state().isEmpty() && cb.others() == 1 && cd.others() == 1; }, 10000));
            CHECK(until([&] { return find("callButton") && find("callButton")->property("text").toString() == "Join (2)"; }, 5000));
            CHECK(click(find("callButton")));
            CHECK(until([&] { return ca.state() == "active" && ca.others() == 2; }, 15000));
            CHECK(clickName("voicePanelLeave"));
            cb.hangUp();
            cd.hangUp();
            CHECK(until([&] { return ca.state().isEmpty() && cb.state().isEmpty() && cd.state().isEmpty() && ca.groupCall(gid) == 0; }, 10000));
            CHECK(until([&] {
                for (QQuickItem *i : all("groupNotice"))
                    if (i->property("text").toString().startsWith("You started a call · 0:0"))
                        return true;
                return false;
            }, 8000));
        }

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
        CHECK(find("appearAs") != nullptr && find("appearAs")->isVisible());                     /* and which profile the group will see */
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
        /* ---- files: pasted into the box (as the picker and a drop add them too), shown waiting, sent with the message ---- */
        {
            CHECK(click(dm_row("Bob")));
            CHECK(until([&] { return A.current() == bob.uid && find("composer") != nullptr; }, 3000));
            const QString photo = home.path() + "/beach.png", notes = home.path() + "/notes.txt";
            QImage img(400, 300, QImage::Format_RGB32);
            img.fill(QColor(30, 120, 200));
            CHECK(img.save(photo, "PNG"));
            {
                QFile f(notes);
                CHECK(f.open(QIODevice::WriteOnly) && f.write("bring towels\n") > 0);
            }
            auto *md = new QMimeData;
            md->setUrls({ QUrl::fromLocalFile(photo), QUrl::fromLocalFile(notes) });
            QGuiApplication::clipboard()->setMimeData(md);
            CHECK(click(find("composer")));
            key(Qt::Key_V, Qt::ControlModifier);                                                     /* paste: files, not their names as text */
            CHECK(until([&] { return all("attachment").size() == 2; }, 3000));
            CHECK(find("composer")->property("text").toString().isEmpty());
            CHECK(until([&] { return find("sendButton")->property("ready").toBool(); }, 5000));        /* each looked at first */
            typeText("for saturday");
            key(Qt::Key_Return);
            B.open(backend.myId());
            auto last_files = [&]() -> QJsonArray {
                return msgs_b->rowCount() ? msgs_b->at(msgs_b->rowCount() - 1).value("files").toArray() : QJsonArray();
            };
            CHECK(until([&] { return last_files().size() == 2; }, 10000));
            {
                const QJsonArray fs = last_files();
                const QJsonObject p0 = fs.at(0).toObject(), p1 = fs.at(1).toObject();
                CHECK(p0.value("name").toString() == "beach.png" && p0.value("mime").toString() == "image/png" && p0.value("w").toInt() == 400 &&
                      p0.value("h").toInt() == 300 && !p0.value("preview").toString().isEmpty());          /* a photo: its size, and a tiny preview */
                CHECK(p1.value("name").toString() == "notes.txt" && p1.value("mime").toString() == "text/plain" && !p1.contains("preview"));
                CHECK(msgs_b->at(msgs_b->rowCount() - 1).value("body").toString() == "for saturday");
                QByteArray pv = QByteArray::fromBase64(p0.value("preview").toString().toLatin1());
                CHECK(pv.size() > 0 && pv.size() <= 1024);
            }
            CHECK(until([&] { return all("attachment").isEmpty(); }, 3000));                          /* the tray is empty again */
            /* shown in the conversation: the photo itself, and the text file as a card */
            CHECK(until([&] { return all("mediaTile").size() == 1 && all("fileCard").size() == 1 && find_text("fileName", "notes.txt"); }, 5000));
            CHECK(until([&] { QQuickItem *i = find("mediaImage"); return i && i->property("status").toInt() == 1; }, 5000));

            /* ---- Bob sends a photo: bigger than the window's Puddle fetches by itself, so it waits for a click ---- */
            const QString sunset = home.path() + "/sunset.png";
            QImage warm(640, 480, QImage::Format_RGB32);
            warm.fill(QColor(230, 120, 40));
            CHECK(warm.save(sunset, "PNG"));
            B.sendFiles("from bob", QString(), { QVariantMap{ { "key", "b1" }, { "path", sunset }, { "name", "sunset.png" }, { "mime", "image/png" },
                                                             { "w", 640 }, { "h", 480 } } });
            CHECK(until([&] { return shows("from bob"); }, 10000));
            CHECK(until([&] { return find("downloadFile") != nullptr; }, 5000));                     /* waiting, with a Download button */
            CHECK(click(find("downloadFile")));
            CHECK(until([&] {
                int ready = 0;
                for (QQuickItem *i : all("mediaImage")) ready += i->property("status").toInt() == 1;
                return ready == 2;
            }, 10000));                                                                                  /* fetched, then shown */
            CHECK(find("downloadFile") == nullptr);

            /* ---- clicked: over the whole window; Escape puts it away ---- */
            QList<QQuickItem *> tiles = all("mediaTile");
            CHECK(tiles.size() == 2 && click(tiles.last()));
            CHECK(until([&] { QQuickItem *i = find("viewerImage"); return i && i->property("status").toInt() == 1; }, 5000));
            CHECK(find("viewerClose") != nullptr);
            key(Qt::Key_Escape);
            CHECK(until([&] { return find("viewerImage") == nullptr; }, 3000));

            /* ---- Bob asks for notes.txt again, after it reached him long ago: the window asks its owner, who sends it ---- */
            {
                auto notes_row = [&](QString *mid, QString *fid, QString *state) {
                    for (int r = 0; r < msgs_b->rowCount(); r++)
                        for (const QJsonValue &v : msgs_b->at(r).value("files").toArray())
                            if (v.toObject().value("name").toString() == "notes.txt") {
                                *mid = msgs_b->at(r).value("id").toString();
                                *fid = v.toObject().value("id").toString();
                                *state = v.toObject().value("state").toString();
                                return true;
                            }
                    return false;
                };
                QString mid, fid, state;
                CHECK(until([&] { return notes_row(&mid, &fid, &state) && state == "available"; }, 10000));
                QFile::remove(QString(bob.cfg.data_dir) + "/files/" + fid);                         /* Bob loses it */
                QFile::remove(QString(bob.cfg.data_dir) + "/files/" + fid + ".chunks");
                char appData[1024];
                CHECK(puddle_config_default_data_dir(appData, sizeof appData) == 0);
                {
                    QFile nf(QString(appData) + "/files/notes/" + fid);                              /* and it reached him long ago */
                    CHECK(nf.open(QIODevice::WriteOnly) && nf.write(QString("served %1 1\n").arg(bob.uid).toUtf8()) > 0);
                }
                B.requestFile(mid, fid);
                CHECK(until([&] { return find("sendAgain") != nullptr && find("fileRequest") != nullptr; }, 10000)); /* "Bob asked for notes.txt again" */
                CHECK(click(find("sendAgain")));
                CHECK(until([&] { return notes_row(&mid, &fid, &state) && state == "available"; }, 15000));        /* he has it again */
                CHECK(until([&] { return find("fileRequest") == nullptr; }, 5000));
            }

            /* ---- a video (only with PUDDLE_TEST_VIDEO=<a short clip>, as making one needs a tool the build does not):
               its first frame, size and length go with it, and it plays in the window ---- */
            const QString clip = QString::fromLocal8Bit(qgetenv("PUDDLE_TEST_VIDEO"));
            if (!clip.isEmpty()) {
                md = new QMimeData;
                md->setUrls({ QUrl::fromLocalFile(clip) });
                QGuiApplication::clipboard()->setMimeData(md);
                CHECK(click(find("composer")));
                key(Qt::Key_V, Qt::ControlModifier);
                CHECK(until([&] { return all("attachment").size() == 1 && find("sendButton")->property("ready").toBool(); }, 10000));
                key(Qt::Key_Return);
                CHECK(until([&] { return last_files().size() == 1 && last_files().at(0).toObject().value("mime").toString().startsWith("video/"); }, 10000));
                const QJsonObject v = last_files().at(0).toObject();
                fprintf(stderr, "video as sent: %s\n", QJsonDocument(v).toJson(QJsonDocument::Compact).left(300).constData());
                CHECK(v.value("w").toInt() > 0 && v.value("h").toInt() > 0 && v.value("duration_ms").toInt() > 0 && !v.value("preview").toString().isEmpty());
                CHECK(until([&] { return all("mediaTile").size() == 3; }, 5000));
                CHECK(click(all("mediaTile").last()));
                CHECK(until([&] { QQuickItem *s = find("seek"); return s && s->property("value").toDouble() > 300; }, 15000));   /* playing */
                key(Qt::Key_Escape);
                CHECK(until([&] { return find("seek") == nullptr; }, 3000));
            }

            /* taken out again before sending; a copied picture becomes a file of its own */
            md = new QMimeData;
            md->setUrls({ QUrl::fromLocalFile(notes) });
            QGuiApplication::clipboard()->setMimeData(md);
            CHECK(click(find("composer")));
            key(Qt::Key_V, Qt::ControlModifier);
            CHECK(until([&] { return all("attachment").size() == 1; }, 3000));
            CHECK(click(find("removeAttachment")));
            CHECK(until([&] { return all("attachment").isEmpty(); }, 3000));
            QGuiApplication::clipboard()->setImage(img);
            CHECK(click(find("composer")));
            key(Qt::Key_V, Qt::ControlModifier);
            CHECK(until([&] { return all("attachment").size() == 1; }, 3000));                            /* saved as a PNG, to go like any file */
            CHECK(click(find("removeAttachment")));
            CHECK(until([&] { return all("attachment").isEmpty(); }, 3000));
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

    /* ---- a Pond (docs/ponds.md), only against a real one: PUDDLE_TEST_POND is an invite to it, and PUDDLE_TEST_PONDCTL the
     * command that runs its pondctl (to give the window's owner a role). Its icon and members' pictures; the screens an
     * administrator has. ---- */
    if (const QString invite = qEnvironmentVariable("PUDDLE_TEST_POND"); !invite.isEmpty()) {
        const QString ctl = qEnvironmentVariable("PUDDLE_TEST_PONDCTL"), shot = qEnvironmentVariable("PUDDLE_SHOT");
        Ponds &P = *A.pondList(), &PB = *B.pondList();
        auto picture = [&](Chats &who, const QString &file, char fill) {
            QFile f(home.path() + "/" + file);
            QImage img(64, 64, QImage::Format_RGB32);
            img.fill(fill == 'a' ? QColor("#d9480f") : fill == 'b' ? QColor("#1971c2") : QColor("#2f9e44"));
            CHECK(img.save(f.fileName(), "PNG"));
            QSignalSpy up(&who, &Chats::pictureUploaded);
            who.uploadPicture(QUrl::fromLocalFile(f.fileName()), file);
            CHECK(until([&] { return up.count() == 1; }, 5000));
            return up.count() ? up.at(0).at(1).toString() : QString();
        };
        const QString mine = picture(A, "me.png", 'a'), bobs = picture(B, "bob.png", 'b'), icon = picture(A, "icon.png", 'c');
        A.saveProfile("pondface", { { "display_name", "Ann" }, { "blurb", "Here for the frogs" }, { "avatar", mine } }, true);
        B.saveProfile("main", { { "display_name", "Bob" }, { "avatar", bobs } }, false);
        QSignalSpy joinedA(&P, &Ponds::joined), joinedB(&PB, &Ponds::joined), failedA(&P, &Ponds::failed), failedB(&PB, &Ponds::failed);
        settle(500);
        PB.join(invite, "main");
        CHECK(until([&] { return joinedB.count() == 1; }, 15000));
        P.join(invite, "pondface");
        CHECK(until([&] { return joinedA.count() == 1; }, 15000));
        if (failedA.count())
            fprintf(stderr, "joining: %s\n", failedA.at(0).at(0).toString().toUtf8().constData());
        const QString pondId = joinedA.count() ? joinedA.at(0).at(0).toString() : QString();
        CHECK(until([&] { return find("pondPage") != nullptr && P.current() == pondId; }, 5000)); /* joining opens it */
        auto *members = static_cast<JsonListModel *>(P.members());
        CHECK(until([&] { return row(members, "name", "Bob").value("avatar").toString() == bobs; }, 15000));
        /* the bar at the bottom shows us as this Pond sees us: Ann and her picture, not the default profile */
        CHECK(until([&] { QQuickItem *i = find("pondSelfAvatar"); return i && i->property("hash").toString() == mine && i->property("name").toString() == "Ann"; }, 10000));
        /* Bob's picture reached our Puddle through the Pond, and shows in the member list */
        CHECK(until([&] {
            for (QQuickItem *i : all("pondMemberAvatar"))
                if (i->property("hash").toString() == bobs && i->property("ready").toBool())
                    return true;
            return false;
        }, 20000));

        /* the owner is made an operator, from pondctl: Server Settings, where the icon is set */
        if (!ctl.isEmpty()) {
            CHECK(system((ctl + " member offer " + backend.myId() + " operator --yes > /dev/null").toUtf8().constData()) == 0);
            CHECK(until([&] { return P.currentOffer() == "operator"; }, 10000));
            P.answerOffer(true);
            CHECK(until([&] { return P.canAdmin(); }, 10000));
            CHECK(P.canIcon());
            P.setIcon(icon);
            CHECK(until([&] { return row(static_cast<JsonListModel *>(PB.list()), "id", pondId).value("icon").toString() == icon; }, 15000));
            CHECK(until([&] { QQuickItem *i = find("pondIcon_" + P.currentName()); return i && i->property("ready").toBool(); }, 15000));
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond.png");
            CHECK(clickName("pondHeader"));
            CHECK(clickName("serverSettings"));
            CHECK(until([&] { QQuickItem *i = find("pondSettingsIcon"); return i && i->property("ready").toBool(); }, 5000));
            CHECK(find("changePondIcon") && find("removePondIcon") && find("removePondIcon")->isEnabled());
            settle(300);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-settings.png");
            CHECK(click(find("removePondIcon")));
            CHECK(until([&] { return P.currentIcon().isEmpty(); }, 10000));
            CHECK(until([&] { return find("pondIcon_" + P.currentName()) == nullptr; }, 5000)); /* its initials again */

            /* invites: made with an expiry and a number of uses, its link copied; listed; revoked, asking first */
            CHECK(clickName("settingsSection_invites"));
            auto *invites = static_cast<JsonListModel *>(P.invites());
            CHECK(until([&] { return invites->rowCount() >= 1; }, 10000)); /* the one we joined with, from pondctl */
            const int before = invites->rowCount();
            QGuiApplication::clipboard()->setText("nothing yet");
            CHECK(clickName("createInvite"));
            CHECK(until([&] { return invites->rowCount() == before + 1; }, 10000));
            CHECK(until([&] { return QGuiApplication::clipboard()->text().startsWith("pond://127.0.0.1:"); }, 5000));
            const QString made = QGuiApplication::clipboard()->text();
            CHECK(find("madeInviteLink") && find("madeInviteLink")->property("text").toString() == made);
            const QJsonObject inv = invites->rowCount() ? invites->at(0) : QJsonObject();
            CHECK(inv.value("link").toString() == made && inv.value("maxUses").toInt() == 0 && inv.value("expires").toDouble() > QDateTime::currentMSecsSinceEpoch() + 6.9 * 86400 * 1000);
            settle(300);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-invites.png");
            QQuickItem *revoke = nullptr;
            for (QQuickItem *r : all("revokeInvite"))
                revoke = revoke ? revoke : r;
            CHECK(revoke && click(revoke));
            CHECK(until([&] { return find("confirmOk") != nullptr; }, 3000));
            CHECK(click(find("confirmOk")));
            CHECK(until([&] { return invites->rowCount() == before; }, 10000));
            key(Qt::Key_Escape);
            settle(300);

            /* someone's card in the Pond: a click on their name in the member list; their picture, blurb and role */
            CHECK(clickName("pondMember_Ann"));
            CHECK(until([&] { QQuickItem *n = find("profileCardName"); return n && n->property("text").toString() == "Ann"; }, 5000));
            CHECK(find("profileCardBlurb") && find("profileCardBlurb")->property("text").toString() == "Here for the frogs");
            CHECK(until([&] { QQuickItem *a = find("profileCardAvatar"); return a && a->property("ready").toBool(); }, 5000));
            settle(300);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-card.png");
            key(Qt::Key_Escape);
            settle(200);

            /* the menu on someone, for an operator: Mute, Kick, Ban; muting asks how long, and Unmute takes it back */
            CHECK(until([&] { return find("pondMember_Bob") != nullptr; }, 3000));
            CHECK(rightClick(find("pondMember_Bob")));
            CHECK(until([&] { return find("personMute") != nullptr; }, 3000));
            CHECK(find("personProfile") && find("personKick") && find("personBan") && !find("personUnmute"));
            settle(200);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-menu.png");
            CHECK(click(find("personMute")));
            CHECK(until([&] { return find("confirmMute") != nullptr; }, 3000));
            CHECK(click(find("confirmMute")));
            CHECK(until([&] { return row(members, "name", "Bob").value("muted").toBool(); }, 10000));
            CHECK(until([&] { return find("pondMemberMuted") != nullptr; }, 3000));
            PB.send("can anyone hear me");
            CHECK(until([&] { return failedB.count() >= 1 && failedB.last().value(0).toString().contains("muted"); }, 5000)); /* Bob is told why */
            CHECK(rightClick(find("pondMember_Bob")));
            CHECK(until([&] { return find("personUnmute") != nullptr; }, 3000));
            CHECK(!find("personMute"));
            CHECK(click(find("personUnmute")));
            CHECK(until([&] { return !row(members, "name", "Bob").value("muted").toBool(); }, 10000));
            /* our own row: nothing to do to ourselves */
            CHECK(rightClick(find("pondMember_Ann")));
            CHECK(until([&] { return find("personProfile") != nullptr; }, 3000));
            CHECK(!find("personKick") && !find("personMute"));
            key(Qt::Key_Escape);
            settle(200);

            /* roles for channels, and the channels' order (the Pond's docs/design.md, "Roles for channels"): a role made in
             * Server Settings; a channel left to it disappears for Bob, appears again once he holds it, and moves for him too
             * when an operator moves it */
            CHECK(P.canRoles());
            auto *roles = static_cast<JsonListModel *>(P.roles());
            auto *chansA = static_cast<JsonListModel *>(P.channels()), *chansB = static_cast<JsonListModel *>(PB.channels());
            auto place = [](JsonListModel *m, const QString &name) {
                for (int i = 0; i < m->rowCount(); i++)
                    if (m->at(i).value("name").toString() == name)
                        return i;
                return -1;
            };
            CHECK(clickName("pondHeader"));
            CHECK(clickName("serverSettings"));
            CHECK(clickName("settingsSection_roles"));
            CHECK(until([&] { return find("newRoleName") != nullptr; }, 3000));
            find("newRoleName")->setProperty("text", "Frog Wranglers");
            CHECK(clickName("createRole"));
            CHECK(until([&] { return roles->rowCount() == 1; }, 10000));
            const QString roleId = roles->at(0).value("id").toString();
            CHECK(P.roleName(roleId) == "Frog Wranglers");
            P.createChannel("wranglers", "");
            CHECK(until([&] { return place(chansB, "wranglers") >= 0; }, 10000)); /* open to everyone at first */
            const QString wid = row(chansA, "name", "wranglers").value("id").toString();
            P.setChannelRoles(wid, false, { roleId });
            CHECK(until([&] { return place(chansB, "wranglers") < 0; }, 10000)); /* gone for Bob */
            CHECK(until([&] { return row(chansA, "name", "wranglers").value("restricted").toBool(); }, 5000)); /* still there for us */
            const QString bobId = row(members, "name", "Bob").value("id").toString();
            CHECK(clickName("settingsSection_members"));
            CHECK(until([&] { return find("memberRoles_Bob") != nullptr; }, 3000));
            P.setMemberRole(bobId, roleId, true);
            CHECK(until([&] { return place(chansB, "wranglers") >= 0; }, 10000)); /* back for Bob, with the role */
            CHECK(until([&] { return row(members, "name", "Bob").value("roles").toArray().contains(roleId); }, 5000));
            CHECK(until([&] { return roles->rowCount() == 1 && roles->at(0).value("count").toInt() == 1; }, 5000));
            /* the order: moved up with its button, for everyone */
            CHECK(clickName("settingsSection_channels"));
            const int was = place(chansA, "wranglers");
            CHECK(was > 0 && until([&] { return find("moveChannelUp_wranglers") != nullptr; }, 3000));
            settle(300);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-channels.png");
            CHECK(click(find("moveChannelUp_wranglers")));
            CHECK(until([&] { return place(chansA, "wranglers") == was - 1; }, 10000));
            CHECK(until([&] { return place(chansB, "wranglers") == place(chansB, chansA->at(was).value("name").toString()) - 1; }, 10000));
            /* the role deleted: the channel is left to operators and administrators */
            P.deleteRole(roleId);
            CHECK(until([&] { return roles->rowCount() == 0 && place(chansB, "wranglers") < 0; }, 10000));
            CHECK(place(chansA, "wranglers") >= 0);
            key(Qt::Key_Escape);
            settle(200);
        }
        /* ---- a direct message through the Pond (docs/pond-dms.md): "Bob @ <Pond>", sealed from one Puddle to the other, with a
         * banner saying so; Bob offers to be friends ---- */
        {
            const QString bobId = row(members, "name", "Bob").value("id").toString();
            QSignalSpy opened(&A, &Chats::pondDmOpened);
            A.openPondDm(pondId, bobId);
            CHECK(until([&] { return opened.count() == 1; }, 10000));
            const QString conv = opened.count() ? opened.at(0).at(0).toString() : QString();
            CHECK(until([&] { return find("pondDmBanner") != nullptr && A.currentIsPondDm(); }, 5000));
            CHECK(A.currentName() == "Bob @ " + A.currentPondDm().value("pondName").toString());
            CHECK(!find("callButton") && !find("contextButton")); /* no calls through a Pond, nor a friend's side panel */
            CHECK(A.currentPondDm().value("friend").toBool() == (A.person(bobId).value("state").toString() == "active"));
            A.send("psst, bob");
            auto *bobConvs = static_cast<JsonListModel *>(B.conversations());
            CHECK(until([&] { return !row(bobConvs, "id", conv).isEmpty(); }, 20000));
            CHECK(row(bobConvs, "id", conv).value("name").toString().startsWith("Ann @ "));
            B.open(conv, "pond_dm");
            CHECK(until([&] { return !row(static_cast<JsonListModel *>(B.messages()), "body", "psst, bob").isEmpty(); }, 10000));
            CHECK(until([&] { return row(static_cast<JsonListModel *>(A.messages()), "body", "psst, bob").value("deliveryState").toString().isEmpty(); }, 15000));
            B.send("hello ann");
            CHECK(until([&] { return !row(static_cast<JsonListModel *>(A.messages()), "body", "hello ann").isEmpty(); }, 15000));
            /* our own messages carry the picture this Pond sees, not the default profile's */
            CHECK(row(static_cast<JsonListModel *>(A.messages()), "body", "psst, bob").value("avatar").toString() == mine);
            CHECK(row(static_cast<JsonListModel *>(A.messages()), "body", "psst, bob").value("senderName").toString() == "Ann");
            B.befriend();
            CHECK(until([&] { return find("pondDmAcceptOffer") != nullptr; }, 15000));
            /* the Pond's icon sits on Bob's picture: in the list, and over the conversation */
            if (P.canAdmin()) {
                P.setIcon(icon);
                CHECK(until([&] { return row(static_cast<JsonListModel *>(A.sidebar()), "id", conv).value("pondIcon").toString() == icon; }, 15000));
                CHECK(until([&] {
                    int ready = 0;
                    for (QQuickItem *i : all("avatarBadge"))
                        ready += i->property("hash").toString() == icon && i->property("ready").toBool();
                    return ready >= 2;
                }, 15000));
            } else {
                CHECK(until([&] { return all("avatarBadge").size() >= 2; }, 5000));
            }
            settle(400);
            if (!shot.isEmpty())
                win->grabWindow().save(shot + "-pond-dm.png");
            /* block his messages: listed in Settings, under Blocks */
            A.dmBlock(bobId);
            CHECK(until([&] { return static_cast<JsonListModel *>(A.dmBlocks())->rowCount() == 1; }, 10000));
            B.close();
            A.close();
        }
        P.leave(pondId);
        PB.leave(pondId);
        CHECK(until([&] { return static_cast<JsonListModel *>(P.list())->rowCount() == 0; }, 10000));
        /* gone from the Pond, the conversation through it keeps Bob's name and picture as they last were */
        A.refresh();
        CHECK(until([&] {
            for (const QJsonObject &r : static_cast<JsonListModel *>(A.sidebar())->items())
                if (r.value("kind").toString() == "pond_dm")
                    return r.value("name").toString().startsWith("Bob @ ") && r.value("avatar").toString() == bobs;
            return false;
        }, 10000));
    }

    /* ---- a phone, let in from the window (docs/identity.md, "Devices") ---- */
    {
        const QString shot = qEnvironmentVariable("PUDDLE_SHOT");
        CHECK(clickName("settingsButton"));
        CHECK(until([&] { return find("addPhoneButton") != nullptr; }, 3000));
        CHECK(clickName("addPhoneButton"));
        CHECK(until([&] { QQuickItem *f = find("pairingLinkField"); return f && f->property("text").toString().startsWith("puddle-pair://"); }, 5000));
        const QString link = find("pairingLinkField") ? find("pairingLinkField")->property("text").toString() : QString();
        CHECK(find("addPhoneButton") == nullptr);                                                  /* one link at a time */
        settle();
        if (!shot.isEmpty())
            win->grabWindow().save(shot + "-phones-link.png");

        /* the phone, shown as it would be on the phone itself */
        QTemporaryDir pd;
        Chats P;
        P.setInterval(100);
        Phone phone(&P, pd.path());
        phone.setPollInterval(100);
        QQmlApplicationEngine pe;
        pe.rootContext()->setContextProperty("thePhone", &phone);
        pe.loadData(QString(R"(
            import QtQuick
            import QtQuick.Controls
            import "file://%1/setup" as S
            ApplicationWindow { width: 390; height: 780; visible: true
                S.Pair { anchors.fill: parent; link: thePhone } })").arg(PUDDLE_GUI_DIR).toUtf8(), QUrl("file:///pair_test.qml"));
        CHECK(!pe.rootObjects().isEmpty());
        auto *pw = pe.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(pe.rootObjects().at(0));
        auto pfind = [&](const QString &name) {
            QQuickItem *hit = nullptr;
            if (pw)
                walk(pw->contentItem(), [&](QQuickItem *i) { if (!hit && i->objectName() == name && i->isVisible()) hit = i; return hit != nullptr; });
            return hit;
        };
        CHECK(until([&] { return pfind("pairLink") != nullptr; }, 3000));
        if (pw && !shot.isEmpty())
            pw->grabWindow().save(shot + "-phone-pair.png");
        if (QQuickItem *f = pfind("pairLink"))
            f->setProperty("text", link);
        CHECK(pfind("pairButton") && pfind("pairButton")->property("enabled").toBool());
        if (QQuickItem *b = pfind("pairButton"))
            QMetaObject::invokeMethod(b, "clicked");
        CHECK(until([&] { return phone.state() == "waiting"; }, 8000));
        CHECK(until([&] { QQuickItem *c = pfind("pairCheck"); return c && c->property("text").toString().remove(' ') == phone.check(); }, 3000));
        if (pw && !shot.isEmpty())
            pw->grabWindow().save(shot + "-phone-waiting.png");

        /* the window asks the owner, with the same six digits, and the owner lets it in */
        CHECK(until([&] { return find("acceptPhone") != nullptr; }, 8000));
        bool digits = false;
        walk(win->contentItem(), [&](QQuickItem *i) {
            digits = digits || (i->isVisible() && i->property("text").toString().contains(phone.check().left(3) + " " + phone.check().mid(3)));
            return false;
        });
        CHECK(digits);
        CHECK(find("pairingLinkField") == nullptr);                                                 /* used: not shown any more */
        settle();
        if (!shot.isEmpty())
            win->grabWindow().save(shot + "-phones-pending.png");
        CHECK(click(find("acceptPhone")));
        CHECK(until([&] { return phone.state() == "ready"; }, 8000));
        CHECK(until([&] { return P.friendCount() == A.friendCount() && P.connected() && P.problem().isEmpty(); }, 8000));
        CHECK(until([&] { return find("acceptPhone") == nullptr && button(QObject::tr("Remove")) != nullptr; }, 5000));

        /* profiles from the phone: the Puddle keeps them, so a picture chosen there goes to the Puddle, and the window sees it */
        {
            const QString png = home.path() + "/pic.png";
            QImage pic(48, 48, QImage::Format_RGB32);
            pic.fill(QColor("#3366cc"));
            CHECK(pic.save(png));
            QSignalSpy up(&P, &Chats::pictureUploaded), saved(&P, &Chats::profileSaved);
            P.uploadPicture(QUrl::fromLocalFile(png), "avatar");
            CHECK(until([&] { return up.count() == 1; }, 10000));
            const QString hash = up.value(0).value(1).toString();
            CHECK(up.value(0).value(0).toString() == "avatar" && hash.size() == 64 && up.value(0).value(2).toString().isEmpty());
            CHECK(until([&] { return !P.profile("phone-made").isEmpty() || P.profileIdFor("Phone Made") == "phone-made"; }, 3000));
            P.saveProfile("phone-made", { { "display_name", "Phone Made" }, { "pronouns", "they/them" }, { "avatar", hash } }, true);
            CHECK(until([&] { return saved.count() == 1; }, 10000) && saved.value(0).value(1).toString().isEmpty());
            CHECK(until([&] { return A.profile("phone-made").value("avatar").toString() == hash; }, 8000)); /* the window has it */
            CHECK(A.profile("phone-made").value("pronouns").toString() == "they/them");
            P.saveProfile("phone-made", { { "display_name", "Phone Made" }, { "pronouns", QVariant() } }, false); /* null clears */
            CHECK(until([&] { return saved.count() == 2; }, 10000) && saved.value(1).value(1).toString().isEmpty());
            CHECK(until([&] { return !A.profile("phone-made").contains("pronouns") || A.profile("phone-made").value("pronouns").isNull(); }, 8000));
            QSignalSpy gone(&P, &Chats::profileDeleted);
            P.deleteProfile("phone-made");
            CHECK(until([&] { return gone.count() == 1 && A.profile("phone-made").isEmpty(); }, 8000));
        }
    }

    {   /* Settings: a new profile, made in the window */
        CHECK(until([&] { return find("newProfile") != nullptr; }, 3000));
        const int before = all("profileRow").size();
        CHECK(before >= 1);
        CHECK(click(find("newProfile")));
        CHECK(until([&] { return find("profileName") != nullptr && find("profileName")->isVisible(); }, 3000));
        if (QQuickItem *f = find("profileName"))
            f->setProperty("text", "Work Me");
        settle();
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
            win->grabWindow().save(shot + "-profile-edit.png");
        CHECK(click(find("profileSave")));
        CHECK(until([&] { return all("profileRow").size() == before + 1; }, 8000));
        if (QObject *d = win->findChild<QObject *>("profileDialog"); d && !d->property("problem").toString().isEmpty())
            fprintf(stderr, "profile dialog says: %s\n", qPrintable(d->property("problem").toString()));
        CHECK(A.profile("work-me").value("display_name").toString() == "Work Me");
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
            win->grabWindow().save(shot + "-profiles.png");
    }

    {   /* Settings: a backup, written while the Puddle runs; then the Puddle moved to a server, with this app as its device */
        CHECK(find("backupButton") != nullptr && find("moveButton") != nullptr);
        /* Settings is taller than the window: it scrolls, with the wheel, to what is below */
        QQuickItem *scroll = find("settingsScroll");
        CHECK(scroll && scroll->property("contentHeight").toDouble() > scroll->height());
        if (scroll) {
            scroll->setProperty("contentY", 0);
            settle(100);
            wheel(find("profilesSettings") ? find("profilesSettings") : scroll, -3);
            CHECK(until([&] { return scroll->property("contentY").toDouble() > 0; }, 2000));
        }
        /* Blocks: both kinds listed, each with Unblock; Bob's messages, blocked through the Pond, are let back */
        {
            auto *dmBlocks = static_cast<JsonListModel *>(A.dmBlocks());
            CHECK(find("blocksSection") != nullptr);
            if (dmBlocks->rowCount() == 0) /* no Pond to meet him in: block Bob's messages from here */
                A.dmBlock(row(static_cast<JsonListModel *>(A.people()), "name", "Bob").value("id").toString());
            CHECK(until([&] { return dmBlocks->rowCount() == 1 && find("dmBlockedRow") != nullptr; }, 5000));
            CHECK(find("dmBlockedRow") && find("dmBlockedRow")->property("name").toString() == "Bob");
            reveal(find("dmBlockedRow"));
            settle(300);
            if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
                win->grabWindow().save(shot + "-blocks.png");
            QQuickItem *unblock = nullptr;
            for (QQuickItem *i : all("personUnblock"))
                for (QQuickItem *p = i; p; p = p->parentItem())
                    if (p->objectName() == "dmBlockedRow")
                        unblock = i;
            CHECK(click(unblock));
            CHECK(until([&] { return dmBlocks->rowCount() == 0 && find("dmBlockedRow") == nullptr; }, 10000));
            if (scroll)
                scroll->setProperty("contentY", 0);
            settle(100);
        }
        /* Levee: none joined; a link that isn't one is said so, and nothing changes */
        {
            CHECK(find("leveeSettings") != nullptr && find("leveeLinkField") != nullptr);
            CHECK(find("pondVoiceDirectSwitch") == nullptr); /* only shown behind a Levee (find sees what is visible) */
            CHECK(until([&] { return !A.leveeJoined(); }, 5000));
            A.joinLevee("puddle://not-a-levee.example.org");
            CHECK(until([&] { return !A.leveeBusy() && !A.leveeError().isEmpty(); }, 10000));
            CHECK(!A.leveeJoined() && find("leveeError") && find("leveeError")->isVisible());
            reveal(find("leveeSettings"));
            settle(200);
            if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty())
                win->grabWindow().save(shot + "-levee.png");
            if (scroll)
                scroll->setProperty("contentY", 0);
            settle(100);
        }
        if (const QString shot = qEnvironmentVariable("PUDDLE_SHOT"); !shot.isEmpty()) {
            win->grabWindow().save(shot + "-settings.png");
            CHECK(click(find("moveButton")));
            QTest::qWait(400);
            win->grabWindow().save(shot + "-move.png");
            key(Qt::Key_Escape);
            QTest::qWait(200);
        }
        QSignalSpy done(&backend, &Backend::backupFinished);
        const QString bk = home.path() + "/mine.puddle", moved = home.path() + "/moved.puddle", dataDir = backend.dataDir();
        CHECK(backend.makeBackup(QUrl::fromLocalFile(bk)));
        CHECK(until([&] { return done.count() == 1; }, 30000));
        CHECK(done.value(0).value(0).toString().isEmpty() && puddle_backup_is(bk.toUtf8().constData()));
        CHECK(backend.running());

        CHECK(!backend.handOver("not an address at all", QUrl::fromLocalFile(moved)) && backend.running()); /* nothing stopped */
        done.clear();
        CHECK(backend.handOver("puddle.example.org:443", QUrl::fromLocalFile(moved)));
        CHECK(until([&] { return done.count() == 1; }, 30000));
        CHECK(done.value(0).value(0).toString().isEmpty() && done.value(0).value(2).toBool());
        CHECK(!backend.running() && !backend.hasLocalPuddle());
        CHECK(!QFileInfo::exists(dataDir) && QFileInfo::exists(dataDir + ".handed-over"));  /* put aside, not deleted */
        CHECK(QSettings().value("mode").toString() == "remote");

        QFile dj(Backend::phoneDir() + "/device.json");
        CHECK(dj.open(QIODevice::ReadOnly));
        const QJsonObject d = QJsonDocument::fromJson(dj.readAll()).object();
        CHECK(QUrl(d.value("base").toString()).host() == "puddle.example.org" && d.value("state").toString() == "active");
        CHECK(d.value("puddle").toString() == backend.myId() && d.value("puddle_key").toString().size() == 128);

        /* restored on "the server": this app is one of its devices, already let in */
        puddle_config server;
        memset(&server, 0, sizeof server);
        snprintf(server.dir, sizeof server.dir, "%s/server", home.path().toUtf8().constData());
        snprintf(server.data_dir, sizeof server.data_dir, "%s/server/data", home.path().toUtf8().constData());
        char err[512] = "";
        CHECK(puddle_backup_restore(&server, moved.toUtf8().constData(), phrase.toUtf8().constData(), nullptr, err, sizeof err) == 0);
        puddle_devkey me;
        const QByteArray seed = QByteArray::fromHex(d.value("seed").toString().toLatin1());
        CHECK(seed.size() == 32 && puddle_devkey_from_seed(&me, reinterpret_cast<const unsigned char *>(seed.constData())) == 0);
        puddle_devices *devs = puddle_devices_open(server.data_dir, err, sizeof err);
        puddle_device got;
        CHECK(devs && puddle_devices_get(devs, me.id, &got) == 0 && got.state == PUDDLE_DEVICE_ACTIVE);
        if (devs)
            puddle_devices_close(devs);
    }

    return failures ? 1 : 0;
}
