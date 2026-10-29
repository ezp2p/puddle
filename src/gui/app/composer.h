#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariant>

#include "jsonlistmodel.h"

class Chats;
class QImage;
class QMediaPlayer;

// The files waiting to go with the next message (docs/files.md): chosen with the file picker, dropped on the chat, or pasted.
// Each is looked at first: its type, its size, and for a photo or a video its width and height, a video's length, and a
// tiny preview (at most 1 KiB, sent in the message for the other side to show until the file arrives). Then send() hands
// them to Chats::sendFiles and shows how each upload is going.
//
// In QML: import Puddle.Files, then Composer { chats: ... }.
class Composer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *chats READ chats WRITE setChats NOTIFY chatsChanged)
    // where the message goes: "" is the open chat; otherwise an owner API send route, such as a Pond channel's
    Q_PROPERTY(QString target MEMBER target_ NOTIFY targetChanged)
    // key, name, size, mime, kind ("image", "video" or "file"), url (the file, to show a photo), thumb (a picture of a video),
    // state ("preparing", "ready", "uploading", "failed"), progress (0 to 1)
    Q_PROPERTY(QObject *items READ items CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)       // there are files, and every one has been looked at
    Q_PROPERTY(bool sending READ sending NOTIFY changed)
public:
    explicit Composer(QObject *parent = nullptr);
    ~Composer() override;

    static constexpr int kMaxFiles = 10;   // as many as a message carries
    static constexpr int kPreviewMax = 1024;

    QObject *chats() const;
    void setChats(QObject *c);
    QObject *items() { return &items_; }
    int count() const { return int(items_.items().size()); }
    bool ready() const;
    bool sending() const { return sending_; }

    Q_INVOKABLE void add(const QVariant &urls);          // a list of file URLs (the picker's selectedFiles, a drop's urls), or one
    Q_INVOKABLE bool canPaste() const;                   // the clipboard holds a picture or files
    Q_INVOKABLE bool paste();
    Q_INVOKABLE void remove(const QString &key);
    Q_INVOKABLE void clear();
    // Send what is waiting, with `text` (which may be empty), to the open chat. False if there is nothing to send yet.
    Q_INVOKABLE bool send(const QString &text, const QString &replyTo);

    // A preview of `img` at most `max` bytes: a tiny JPEG, as base64. "" if it cannot be made that small.
    static QString previewOf(const QImage &img, int max = kPreviewMax);
    // What to call a file in a message: no folders, no control characters, at most 255 bytes.
    static QString safeName(const QString &name);

signals:
    void chatsChanged();
    void targetChanged();
    void changed();
    void refused(const QString &why);   // a file that cannot be added, said in words
    void sent();
    void failed(const QString &why);

private:
    void addFile(const QString &path);
    void look(const QString &key, const QString &path, const QString &mime);
    void lookAtVideo(const QString &key, const QString &path);
    void update(const QString &key, const QJsonObject &changes);
    QJsonObject item(const QString &key) const;

    JsonListModel items_;
    QPointer<Chats> chats_;
    QString target_;
    QHash<QString, QJsonObject> extra_;          // per key: path, w, h, duration_ms, preview (sent, not shown)
    QHash<QString, QMediaPlayer *> players_;     // videos being looked at
    int next_ = 0;
    bool sending_ = false;
    QString tempDir_;
};
