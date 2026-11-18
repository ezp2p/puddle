#pragma once

#include <QFileSystemWatcher>
#include <QTimer>
#include <QUrl>
#include <QObject>
#include <QVariantMap>
#include <QStringListModel>
#include <memory>
#include <thread>

#include "callaudio.h"
#include "callvideo.h"
#include "chats.h"
#include "phone.h"

extern "C" {
#include "core/config.h"
#include "service/server.h"
}

// C++ side of the UI: owns the embedded server and exposes state to QML.
//
// Start-up has three outcomes: the server runs; there is an error to show; or the Puddle has no usable
// identity yet and the app must walk the owner through making one (needsSetup). In that case the server
// is started by finishSetup(), only after the owner has confirmed writing the recovery phrase down.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString listenUrl READ listenUrl NOTIFY startedChanged)
    Q_PROPERTY(bool running READ running NOTIFY startedChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)   // non-empty if something failed
    Q_PROPERTY(QObject *log READ log CONSTANT)                 // list model, role "display"
    Q_PROPERTY(QObject *chats READ chats CONSTANT)             // people, conversations, messages (see Chats)
    Q_PROPERTY(QObject *video READ video CONSTANT)             // a call's video (see CallVideo)
    Q_PROPERTY(QString myLink READ myLink NOTIFY startedChanged)  // puddle:// link others use to ask us; "" if unknown
    Q_PROPERTY(QString myId READ myId NOTIFY startedChanged)
    // Shows what is normally hidden (user IDs, codes), as Discord's Developer Mode does. Off unless the owner turns it on.
    Q_PROPERTY(bool developerMode READ developerMode WRITE setDeveloperMode NOTIFY developerModeChanged)
    Q_PROPERTY(bool needsSetup READ needsSetup NOTIFY needsSetupChanged)
    // An earlier setup showed a phrase but was never confirmed; it will be started again.
    Q_PROPERTY(bool setupRedo READ setupRedo NOTIFY needsSetupChanged)
    // True once the app has been asked to quit (Cmd+Q, session end, ...), so the
    // window knows to really close instead of hiding to the tray.
    Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
    // The owner's restyling: the `key: value` lines of theme.yaml beside puddle.yaml (empty if there is none). Read again when the file changes.
    Q_PROPERTY(QVariantMap themeValues READ themeValues NOTIFY themeChanged)
    // The app on a phone (Android, or PUDDLE_PHONE=1 to try it on a desktop): no Puddle of its own, a thin client of the owner's
    // (docs/identity.md, "Devices"). `phoneLink` is the pairing; `running` means paired and let in.
    Q_PROPERTY(bool phone READ phone NOTIFY modeChanged)
    Q_PROPERTY(QObject *phoneLink READ phoneLink NOTIFY modeChanged)
    // What this app is, chosen on the first run and remembered: "here" (it runs the owner's Puddle), "remote" (it uses one that
    // runs somewhere else, as a phone does), or "" (not chosen yet: the window asks). `modeFixed`: no choice (a phone).
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(bool modeFixed READ modeFixed CONSTANT)
    Q_PROPERTY(bool hasLocalPuddle READ hasLocalPuddle NOTIFY modeChanged) // an identity is kept here, whatever the mode
    Q_PROPERTY(QString dataDir READ dataDir NOTIFY modeChanged)
    // A backup or a handover being written (they can take a while, with many files): the window waits for `backupFinished`.
    Q_PROPERTY(bool backingUp READ backingUp NOTIFY backingUpChanged)
    Q_PROPERTY(bool canDrawQr READ canDrawQr CONSTANT) // "image://qr/..." works (libqrencode was found)
    // Voice channels (docs/ponds.md, "Voice channels"): how the microphone goes there, kept between runs. `talkMode` is "voice"
    // (voice activation, the default) or "push" (push to talk, while `pushKey` is held: a Qt::Key, 0 for none chosen yet);
    // `talkThreshold` is how loud speech must be for voice activation, in dBFS.
    Q_PROPERTY(QString talkMode READ talkMode WRITE setTalkMode NOTIFY talkChanged)
    Q_PROPERTY(double talkThreshold READ talkThreshold WRITE setTalkThreshold NOTIFY talkChanged)
    Q_PROPERTY(int pushKey READ pushKey WRITE setPushKey NOTIFY talkChanged)
    Q_PROPERTY(QString pushKeyName READ pushKeyName NOTIFY talkChanged)
    // In a voice channel: who is talking now (members' ids; "self" for this end), and how loud the microphone is (dBFS)
    Q_PROPERTY(QStringList speaking READ speaking NOTIFY speakingChanged)
    // How the video pane orders its tiles: "arrival" (as they came, nobody moves; the default) or "speaking" (those talking first)
    Q_PROPERTY(QString videoOrder READ videoOrder WRITE setVideoOrder NOTIFY videoOrderChanged)
    Q_PROPERTY(double micLevel READ micLevel NOTIFY speakingChanged)
public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QString listenUrl() const { return listenUrl_; }
    bool running() const { return phone_ ? phone_->state() == QLatin1String("ready") : srv_ != nullptr; }
    bool phone() const { return phone_ != nullptr; }
    QString mode() const { return mode_; }
    bool modeFixed() const { return phoneMode(); }
    bool hasLocalPuddle() const;
    bool backingUp() const { return backingUp_; }
    QString dataDir() const { return haveConfig_ ? QString::fromUtf8(cfg_.data_dir) : QString(); }
    bool canDrawQr() const;
    QObject *phoneLink() { return phone_; }
    // Whether the app must run as a phone (Android, --phone, PUDDLE_PHONE), and where a phone keeps what it knows.
    static bool phoneMode();
    static QString phoneDir();
    QString error() const { return error_; }
    QObject *log() { return &log_; }
    Chats *chats() { return &chats_; }
    QObject *video() { return &video_; }
    QString myLink() const { return phone_ ? chats_.myLink() : myLink_; }
    QString myId() const { return phone_ ? phone_->puddleId() : myId_; }
    bool developerMode() const { return developerMode_; }
    void setDeveloperMode(bool on);
    QString talkMode() const { return talkMode_; }
    void setTalkMode(const QString &mode);
    double talkThreshold() const { return audio_.gateThreshold(); }
    void setTalkThreshold(double dbfs);
    int pushKey() const { return pushKey_; }
    void setPushKey(int key);
    QString pushKeyName() const;
    QStringList speaking() const { return speaking_; }
    QString videoOrder() const { return videoOrder_; }
    void setVideoOrder(const QString &order);
    double micLevel() const { return micLevel_; }
    bool needsSetup() const { return needsSetup_; }
    bool setupRedo() const { return setupRedo_; }
    bool quitting() const { return quitting_; }
    QVariantMap themeValues() const { return theme_; }
    // Parse the lines of a theme file: `key: value`, `#` starts a comment, quotes around a value are dropped. (A subset of YAML; colours
    // are quoted in the docs because a bare `#` starts a comment in YAML proper, though this reader lets one through.)
    static QVariantMap parseTheme(const QByteArray &text);

    void append(const QString &line);

    // Make the identity and return its recovery phrase (empty on failure, with `error` set). The phrase
    // is returned once and kept nowhere. An earlier unfinished identity is thrown away first.
    Q_INVOKABLE QString createIdentity();

    // The owner has written the phrase down: confirm the identity and start the Puddle.
    Q_INVOKABLE bool finishSetup();

    // Bring the Puddle back here, then start it: from a backup (core/backup.h) and the phrase that sealed it, which brings back
    // everything; or from a copy of its chain (identity/chain.jsonl, not secret) and the latest phrase, which brings back the
    // identity alone. "" when done, else what went wrong.
    Q_INVOKABLE QString restoreIdentity(const QString &phrase, const QUrl &chainFile);

    // Write the Puddle that runs here to `file` (core/backup.h), on another thread: backupFinished says how it went.
    Q_INVOKABLE bool makeBackup(const QUrl &file);
    // Move the Puddle that runs here to a server (docs/backup.md, "Moving to a server"): it stops here for good, this app is
    // added to it as a device, and it is written to `file` for the server to restore; then this app is set to use it at
    // `address` (host:port) after a restart. backupFinished says how it went; the Puddle here starts again if it failed.
    Q_INVOKABLE bool handOver(const QString &address, const QUrl &file);
    // Start the app again (after a handover).
    Q_INVOKABLE bool restartApp();

    // The first run's answer: "here" or "remote". Remembered, so it is asked once.
    Q_INVOKABLE bool choose(const QString &mode);
    // Back to the question, while nothing has been made yet for the answer (no identity confirmed here, no phone paired).
    Q_INVOKABLE bool unchoose();
    // Later, from Settings: ask again, which takes a restart. The Puddle kept here, if any, is left as it is, and comes back if
    // "here" is chosen again. False if the app could not start again.
    Q_INVOKABLE bool askAgain();

    Q_INVOKABLE void copyText(const QString &text);

    // True exactly once per install: the "still running in the tray" hint.
    Q_INVOKABLE bool takeFirstHideNotice();

    // macOS: the unread count on the Dock icon. No-op elsewhere.
    Q_INVOKABLE void setDockBadge(int count);

    // macOS: Dock icon only while the window is shown. No-op on other platforms.
    Q_INVOKABLE void setDockVisible(bool visible);

    // A notification from the system's own centre (macOS), about `who` (a user or group id, or ""). False where there is none,
    // and the caller uses the tray's message instead. Clicking it emits notificationClicked(who).
    Q_INVOKABLE bool notify(const QString &title, const QString &body, const QString &who = QString());

signals:
    void notificationClicked(const QString &who);
    void quittingChanged();
    void developerModeChanged();
    void startedChanged();
    void errorChanged();
    void needsSetupChanged();
    void modeChanged();
    void backingUpChanged();
    void backupFinished(const QString &error, const QString &file, bool handover);
    void themeChanged();
    void talkChanged();
    void speakingChanged();
    void videoOrderChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Phone *phone_ = nullptr;
    void startServer();
    void stopServer();
    QString retireHere(const QString &address, const QByteArray &seed); // after a handover's backup: this Puddle is put away
    void startHere();   // the owner's Puddle runs in this app: load the config, then set up or start
    void startRemote(); // the app uses a Puddle elsewhere
    static QString savedMode();
    static void saveMode(const QString &mode);
    void applyLogLevel();
    void setError(const QString &message);
    void loadTheme();
    void followCall(); // the sound starts when a call connects, or a voice channel does, and stops when it ends
    void applyTalk();  // the talk settings, to the gate, when in a voice channel
    void sampleSpeaking();

    QVariantMap theme_;
    QString themePath_;
    QFileSystemWatcher themeWatch_;
    QStringListModel log_;
    Chats chats_;
    QString listenUrl_, myLink_, myId_;
    QString error_;
    bool quitting_ = false;
    bool developerMode_ = false;
    bool needsSetup_ = false;
    bool setupRedo_ = false;
    bool haveConfig_ = false;
    QString mode_;
    QString configError_;
    bool backingUp_ = false;  // why puddle.yaml couldn't be read, shown if the Puddle is to run here
    puddle_config cfg_;
    puddle_server *srv_ = nullptr;
    std::thread worker_;
    QThreadPool takePool_;    // files handed to the Puddle (Chats::setTakeFile), one at a time
    CallAudio audio_;         // a call's microphone and speaker; its frames go through the Puddle (federation/calls.h)
    CallVideo video_;         // and its camera and pictures
    QString soundFor_;        // what the sound is running for: "call", "voice", or "" (none)
    QString talkMode_ = QStringLiteral("voice");
    QString videoOrder_ = QStringLiteral("arrival");
    int pushKey_ = 0;
    QStringList speaking_;
    double micLevel_ = -100;
    QTimer speakTimer_;       // while in a voice channel: who is talking
};
