#include "backend.h"

#include "federation/calls.h"

#include <QStandardPaths>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSettings>

#include "dock.h"

extern "C" {
#include "core/identity.h"
#include "http/router.h"
#include "base/log.h"
}

namespace {

constexpr int kMaxLogLines = 1000;
Backend *g_self;

void logSink(puddle_log_level level, const char *msg, void *)
{
    static const char *const names[] = { "info", "warn", "error" };
    const QString line = QStringLiteral("%1: %2").arg(QLatin1String(names[level]), QString::fromUtf8(msg));
    // Called from connection threads; hop to the GUI thread.
    if (Backend *self = g_self)
        QMetaObject::invokeMethod(self, [self, line] { self->append(line); }, Qt::QueuedConnection);
}

} // namespace

Backend::Backend(QObject *parent) : QObject(parent)
{
    qApp->installEventFilter(this);
    developerMode_ = QSettings().value("developerMode", false).toBool();
    // asks the owner once, at launch, rather than when the first message arrives while they are away
    puddle_notify_init([](const char *who, void *ctx) { emit static_cast<Backend *>(ctx)->notificationClicked(QString::fromUtf8(who)); }, this);

    char cfgPath[PUDDLE_PATH_MAX];
    if (puddle_config_default_path(cfgPath, sizeof cfgPath) == 0) {
        themePath_ = QFileInfo(QString::fromUtf8(cfgPath)).absoluteDir().filePath("theme.yaml");
        // watch the folder too, so a theme file made while the app runs is noticed
        connect(&themeWatch_, &QFileSystemWatcher::fileChanged, this, &Backend::loadTheme);
        connect(&themeWatch_, &QFileSystemWatcher::directoryChanged, this, &Backend::loadTheme);
        themeWatch_.addPath(QFileInfo(themePath_).absolutePath());
        loadTheme();
    }

    connect(chats_.callModel(), &Calls::changed, this, &Backend::followCall);
    connect(chats_.callModel(), &Calls::mutedChanged, this, [this] { audio_.setMuted(chats_.callModel()->muted()); });
    connect(chats_.callModel(), &Calls::cameraChanged, this, &Backend::followCall);
    connect(chats_.pondList(), &Ponds::voiceChanged, this, &Backend::followCall);
    {
        QSettings s;
        talkMode_ = s.value("voice/talkMode", "voice").toString() == QLatin1String("push") ? QStringLiteral("push") : QStringLiteral("voice");
        pushKey_ = s.value("voice/pushKey", 0).toInt();
        audio_.gate().setThreshold(qBound(-80.0, s.value("voice/threshold", TalkGate::kDefaultThreshold).toDouble(), 0.0));
    }
    speakTimer_.setInterval(150);
    connect(&speakTimer_, &QTimer::timeout, this, &Backend::sampleSpeaking);
    connect(&video_, &CallVideo::cameraFailed, this, [this](const QString &why) { append(QStringLiteral("warning: call: camera: ") + why); });

    if (phoneMode()) { /* no Puddle here: this is a phone of the owner's, once paired */
        phone_ = new Phone(&chats_, phoneDir(), this);
        connect(phone_, &Phone::changed, this, &Backend::startedChanged);
        connect(&chats_, &Chats::profilesChanged, this, &Backend::startedChanged); /* myLink, from the Puddle */
        return;
    }

    char err[512];
    if (puddle_config_load(&cfg_, nullptr, err, sizeof err) != 0) {
        error_ = QString::fromUtf8(err);
        return;
    }
    haveConfig_ = true;

    // No identity, or one whose phrase was shown but never confirmed: setup comes first, and the
    // server stays off until the owner has written the phrase down.
    setupRedo_ = puddle_identity_unconfirmed(cfg_.data_dir) != 0;
    needsSetup_ = setupRedo_ || !puddle_identity_exists(cfg_.data_dir);
    if (!needsSetup_)
        startServer();
}

// Frames from the friend, on one of the Puddle's network threads: into the jitter buffer, which is safe for that.
static void onCallFrame(void *user, const char *, const char *from, const unsigned char *frame, size_t len, uint32_t rtpTs)
{
    static_cast<CallAudio *>(user)->received(QString::fromLatin1(from), rtpTs, QByteArray(reinterpret_cast<const char *>(frame), qsizetype(len)));
}

// A sender's video, on a network thread: to the decoder's thread.
static void onCallVideo(void *user, const char *, const char *from, const unsigned char *au, size_t len, uint32_t rtpTs)
{
    static_cast<CallVideo *>(user)->received(QString::fromLatin1(from), rtpTs, QByteArray(reinterpret_cast<const char *>(au), qsizetype(len)));
}

// The sound runs while this device has an active call: through the Puddle in this process (the window), or a phone's leg.
// So does the video, with the camera on when the owner turns it on. Or while the window is in a Pond's voice channel (one at
// a time: the Puddle never has both), through the Puddle, with the microphone going as the owner chose (applyTalk).
void Backend::followCall()
{
    Calls *calls = chats_.callModel();
    Ponds *ponds = chats_.pondList();
    CallLeg *leg = calls->leg();
    puddle_calls *engine = srv_ ? puddle_server_calls(srv_) : nullptr;
    const bool callLive = calls->state() == QLatin1String("active") && calls->here() && (engine || leg);
    const bool voiceLive = !callLive && engine && ponds->voiceState() == QLatin1String("active");
    const QString want = callLive ? QStringLiteral("call") : voiceLive ? QStringLiteral("voice") : QString();
    if (audio_.running() && want != soundFor_) { /* ended, or the one changed for the other: stop, then start what is wanted */
        if (soundFor_ == QLatin1String("call") && leg) {
            leg->setOnFrame(nullptr);
            leg->setOnVideo(nullptr);
        } else if (engine) {
            puddle_calls_on_audio(engine, nullptr, nullptr);
            puddle_calls_on_video(engine, nullptr, nullptr);
        }
        audio_.stop();
        video_.stop();
        speakTimer_.stop();
        soundFor_.clear();
        sampleSpeaking();
    }
    if (voiceLive) {
        if (!audio_.running()) {
            soundFor_ = want;
            puddle_calls_on_audio(engine, onCallFrame, &audio_);
            audio_.start([engine](const QByteArray &frame) {
                puddle_voice_send_audio(engine, reinterpret_cast<const unsigned char *>(frame.constData()), size_t(frame.size()));
            });
            speakTimer_.start();
            append(QStringLiteral("info: voice: sound on (microphone %1, speaker %2)")
                       .arg(audio_.hasInput() ? QStringLiteral("yes") : QStringLiteral("none"), audio_.hasOutput() ? QStringLiteral("yes") : QStringLiteral("none")));
        }
        audio_.setMuted(ponds->voiceMuted());
        applyTalk();
        return;
    }
    const bool live = callLive;
    if (live && !audio_.running()) {
        soundFor_ = want;
        audio_.gate().setMode(TalkGate::Open); /* a call: the microphone always goes, unless muted */
        audio_.setMuted(calls->muted());
        if (leg) {
            leg->setOnFrame([a = &audio_](const QString &from, uint32_t ts, const QByteArray &frame) { a->received(from, ts, frame); });
            audio_.start([leg](const QByteArray &frame) { leg->send(frame); });
            leg->setOnVideo([v = &video_](const QString &from, uint32_t ts, const QByteArray &au) { v->received(from, ts, au); });
            video_.start([leg](const QByteArray &au) { leg->sendVideo(au); });
        } else {
            const QByteArray id = calls->callId().toUtf8();
            puddle_calls_on_audio(engine, onCallFrame, &audio_);
            audio_.start([engine, id](const QByteArray &frame) {
                puddle_call_send_audio(engine, id.constData(), reinterpret_cast<const unsigned char *>(frame.constData()), size_t(frame.size()));
            });
            puddle_calls_on_video(engine, onCallVideo, &video_);
            video_.start([engine, id](const QByteArray &au) {
                puddle_call_send_video(engine, id.constData(), reinterpret_cast<const unsigned char *>(au.constData()), size_t(au.size()));
            });
        }
        append(QStringLiteral("info: call: sound on (microphone %1, speaker %2%3)")
                .arg(audio_.hasInput() ? QStringLiteral("yes") : QStringLiteral("none"), audio_.hasOutput() ? QStringLiteral("yes") : QStringLiteral("none"),
                     CallAudio::built() ? QString() : QStringLiteral("; built without Opus, so silent")));
    }
    video_.setCamera(live && calls->camera());
}

void Backend::applyTalk()
{
    if (soundFor_ != QLatin1String("voice"))
        return;
    audio_.gate().setMode(talkMode_ == QLatin1String("push") ? TalkGate::PushToTalk : TalkGate::VoiceActivity);
}

// Who is talking in the voice channel, from the frames arriving (and this end, from its own gate), a few times a second.
void Backend::sampleSpeaking()
{
    QStringList now;
    double level = -100;
    if (soundFor_ == QLatin1String("voice")) {
        now = audio_.speaking();
        if (audio_.talking())
            now << QStringLiteral("self");
        level = audio_.gate().lastLevel();
    }
    now.sort();
    if (now == speaking_ && qAbs(level - micLevel_) < 1)
        return;
    speaking_ = now;
    micLevel_ = level;
    emit speakingChanged();
}

void Backend::setTalkMode(const QString &mode)
{
    const QString m = mode == QLatin1String("push") ? QStringLiteral("push") : QStringLiteral("voice");
    if (m == talkMode_)
        return;
    talkMode_ = m;
    QSettings().setValue("voice/talkMode", m);
    audio_.gate().setPushed(false);
    applyTalk();
    emit talkChanged();
}

void Backend::setTalkThreshold(double dbfs)
{
    dbfs = qBound(-80.0, dbfs, 0.0);
    if (qAbs(dbfs - audio_.gateThreshold()) < 0.01)
        return;
    audio_.gate().setThreshold(dbfs);
    QSettings().setValue("voice/threshold", dbfs);
    emit talkChanged();
}

void Backend::setPushKey(int key)
{
    if (key == pushKey_)
        return;
    pushKey_ = key;
    QSettings().setValue("voice/pushKey", key);
    emit talkChanged();
}

QString Backend::pushKeyName() const
{
    return pushKey_ ? QKeySequence(pushKey_).toString(QKeySequence::NativeText) : QString();
}

bool Backend::canDrawQr() const
{
#ifdef PUDDLE_HAVE_QRENCODE /* set on the app when libqrencode is found; see qrimage.h */
    return true;
#else
    return false;
#endif
}

bool Backend::phoneMode()
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return qEnvironmentVariableIsSet("PUDDLE_PHONE") || QCoreApplication::arguments().contains(QStringLiteral("--phone"));
#endif
}

QString Backend::phoneDir()
{
    const QString d = qEnvironmentVariable("PUDDLE_PHONE_DIR");
    return d.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/phone") : d;
}

QVariantMap Backend::parseTheme(const QByteArray &text)
{
    QVariantMap out;
    for (const QByteArray &raw : text.split('\n')) {
        QString line = QString::fromUtf8(raw).trimmed();
        const int colon = line.indexOf(':');
        if (line.startsWith('#') || colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        QString value = line.mid(colon + 1).trimmed();
        if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'')) {
            const int end = value.indexOf(value[0], 1);
            value = end > 0 ? value.mid(1, end - 1) : value.mid(1);
        } else {
            const int hash = value.indexOf(" #");
            if (hash >= 0)
                value = value.left(hash).trimmed();
        }
        if (!key.isEmpty() && !value.isEmpty())
            out.insert(key, value);
    }
    return out;
}

void Backend::loadTheme()
{
    QVariantMap now;
    QFile f(themePath_);
    if (f.open(QIODevice::ReadOnly))
        now = parseTheme(f.read(1 << 16));
    // an editor that saves by replacing the file drops it from the watch list
    if (QFileInfo::exists(themePath_) && !themeWatch_.files().contains(themePath_))
        themeWatch_.addPath(themePath_);
    if (now != theme_) {
        theme_ = now;
        emit themeChanged();
    }
}

Backend::~Backend()
{
    if (CallLeg *leg = chats_.callModel()->leg()) { // a phone: no more frames for a speaker that is going
        leg->setOnFrame(nullptr);
        leg->setOnVideo(nullptr);
        audio_.stop();
        video_.stop();
    }
    if (srv_) {
        puddle_calls_on_audio(puddle_server_calls(srv_), nullptr, nullptr); // no more frames for a speaker that is going
        puddle_calls_on_video(puddle_server_calls(srv_), nullptr, nullptr);
        audio_.stop();
        video_.stop();
        chats_.setTakeFile({}, nullptr);
        takePool_.waitForDone();
        puddle_server_stop(srv_);
        worker_.join(); // run() drains in-flight connections before returning
        puddle_log_set_sink(nullptr, nullptr);
        g_self = nullptr;
        puddle_server_destroy(srv_);
    }
}

void Backend::setError(const QString &message)
{
    if (error_ == message)
        return;
    error_ = message;
    emit errorChanged();
}

void Backend::startServer()
{
    char err[512];
    g_self = this;
    puddle_log_set_sink(logSink, nullptr);

    srv_ = puddle_server_create(&cfg_, err, sizeof err);
    if (!srv_) {
        setError(QString::fromUtf8(err));
        puddle_log_set_sink(nullptr, nullptr);
        g_self = nullptr;
        return;
    }

    const QString addr = QString::fromUtf8(cfg_.address);
    listenUrl_ = (addr.contains(':') ? QStringLiteral("https://[%1]:%2") : QStringLiteral("https://%1:%2"))
                     .arg(addr).arg(cfg_.port);
    worker_ = std::thread([s = srv_] { puddle_server_run(s); });
    takePool_.setMaxThreadCount(1);
    chats_.setTakeFile([s = srv_](int fd, char id[65]) { return puddle_server_take_file(s, fd, id); }, &takePool_);

    // The window reads everything through the owner API, as a phone client does. "All interfaces" is reached by loopback.
    QString host = addr;
    if (host == QLatin1String("::") || host == QLatin1String("0.0.0.0"))
        host = host == QLatin1String("::") ? QStringLiteral("::1") : QStringLiteral("127.0.0.1");
    const QString base = (host.contains(':') ? QStringLiteral("https://[%1]:%2") : QStringLiteral("https://%1:%2")).arg(host).arg(cfg_.port);
    {
        char sid[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(puddle_server_identity(srv_), sid);
        chats_.setSelf(QString::fromLatin1(sid));
    }
    if (!chats_.attach(QUrl(base), QString::fromUtf8(cfg_.data_dir) + "/state/owner-token", QString::fromUtf8(cfg_.dir) + "/cert.pem"))
        setError(QStringLiteral("Could not open the owner token or certificate to talk to your Puddle."));
    char id[PUDDLE_USER_ID_LEN + 1], link[512];
    puddle_identity_id(puddle_server_identity(srv_), id);
    myId_ = QString::fromLatin1(id);
    myLink_ = puddle_make_link(&cfg_, nullptr, link, sizeof link) ? QString::fromUtf8(link) : QString();
    emit startedChanged();
}

QString Backend::createIdentity()
{
    if (!haveConfig_ || !needsSetup_)
        return QString();
    char err[512], phrase[PUDDLE_PHRASE_LEN + 1];
    setError(QString());

    if (puddle_identity_unconfirmed(cfg_.data_dir) &&
        puddle_identity_discard_unconfirmed(cfg_.data_dir, err, sizeof err) < 0) {
        setError(QString::fromUtf8(err));
        return QString();
    }
    puddle_identity *id = puddle_identity_create(cfg_.data_dir, phrase, err, sizeof err);
    if (!id) {
        setError(QString::fromUtf8(err));
        return QString();
    }
    puddle_identity_close(id);

    const QString result = QString::fromLatin1(phrase);
    memset(phrase, 0, sizeof phrase);
    return result;
}

bool Backend::finishSetup()
{
    if (!haveConfig_ || !needsSetup_)
        return false;
    if (puddle_identity_confirm(cfg_.data_dir) != 0) {
        setError(QStringLiteral("Could not save your identity. Check that the data folder can be written to."));
        return false;
    }
    needsSetup_ = false;
    setupRedo_ = false;
    emit needsSetupChanged();
    startServer();
    return running();
}

void Backend::copyText(const QString &text)
{
    if (QClipboard *c = QGuiApplication::clipboard())
        c->setText(text);
}

bool Backend::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Quit && !quitting_) {
        quitting_ = true;
        emit quittingChanged();
    }
    /* push to talk: the chosen key, held anywhere in the window (only while the window is in front: a key held elsewhere is
     * the system's to tell, and it doesn't tell everyone) */
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && pushKey_ && talkMode_ == QLatin1String("push")) {
        auto *k = static_cast<QKeyEvent *>(event);
        if (k->key() == pushKey_ && !k->isAutoRepeat() && watched->isWindowType())
            audio_.gate().setPushed(event->type() == QEvent::KeyPress);
    }
    if (event->type() == QEvent::ApplicationDeactivate)
        audio_.gate().setPushed(false); /* a key let go while the window wasn't looking is let go */
    return QObject::eventFilter(watched, event);
}

void Backend::setDeveloperMode(bool on)
{
    if (developerMode_ == on)
        return;
    developerMode_ = on;
    QSettings().setValue("developerMode", on);
    emit developerModeChanged();
}

void Backend::setDockBadge(int count)
{
    puddle_set_dock_badge(count);
}

void Backend::setDockVisible(bool visible)
{
    puddle_set_dock_visible(visible);
}

bool Backend::notify(const QString &title, const QString &body, const QString &who)
{
    return puddle_notify(title.toUtf8().constData(), body.toUtf8().constData(), who.toUtf8().constData());
}

bool Backend::takeFirstHideNotice()
{
    QSettings settings;
    if (settings.value("trayNoticeShown", false).toBool())
        return false;
    settings.setValue("trayNoticeShown", true);
    return true;
}

void Backend::append(const QString &line)
{
    const int row = log_.rowCount();
    log_.insertRows(row, 1);
    log_.setData(log_.index(row), line);
    if (log_.rowCount() > kMaxLogLines)
        log_.removeRows(0, log_.rowCount() - kMaxLogLines);
}
