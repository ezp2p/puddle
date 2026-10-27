#include "composer.h"

#include "chats.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QMimeData>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

Composer::Composer(QObject *parent)
    : QObject(parent),
      items_({ "key", "name", "size", "mime", "kind", "url", "thumb", "state", "progress" }, this, QStringLiteral("key"))
{
    tempDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/composer";
    connect(&items_, &JsonListModel::countChanged, this, &Composer::changed);
}

Composer::~Composer()
{
    for (QMediaPlayer *p : players_)
        delete p;
}

QObject *Composer::chats() const { return chats_; }

void Composer::setChats(QObject *c)
{
    Chats *ch = qobject_cast<Chats *>(c);
    if (ch == chats_)
        return;
    if (chats_)
        disconnect(chats_, nullptr, this, nullptr);
    chats_ = ch;
    if (ch) {
        connect(ch, &Chats::uploadProgress, this, [this](const QString &key, double f) { update(key, { { "state", "uploading" }, { "progress", f } }); });
        connect(ch, &Chats::filesSent, this, [this] {
            if (!sending_)
                return;
            sending_ = false;
            clear();
            emit sent();
        });
        connect(ch, &Chats::filesFailed, this, [this](const QString &why) {
            if (!sending_)
                return;
            sending_ = false;
            QList<QJsonObject> rows = items_.items();
            for (QJsonObject &o : rows)
                if (o.value("state").toString() == QLatin1String("uploading"))
                    o.insert("state", "failed");
            items_.setItems(rows);
            emit changed();
            emit failed(why);
        });
    }
    emit chatsChanged();
}

bool Composer::ready() const
{
    if (items_.items().isEmpty())
        return false;
    for (const QJsonObject &o : items_.items())
        if (o.value("state").toString() == QLatin1String("preparing"))
            return false;
    return true;
}

QJsonObject Composer::item(const QString &key) const
{
    for (const QJsonObject &o : items_.items())
        if (o.value("key").toString() == key)
            return o;
    return {};
}

void Composer::update(const QString &key, const QJsonObject &changes)
{
    QList<QJsonObject> rows = items_.items();
    for (QJsonObject &o : rows)
        if (o.value("key").toString() == key)
            for (auto it = changes.begin(); it != changes.end(); ++it)
                o.insert(it.key(), it.value());
    items_.setItems(rows);
    emit changed();
}

QString Composer::safeName(const QString &name)
{
    QString n = name;
    n.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f/\\\\]")));
    n = n.trimmed();
    if (n.isEmpty() || n == QLatin1String(".") || n == QLatin1String(".."))
        n = QStringLiteral("file");
    while (n.toUtf8().size() > 255)
        n.chop(1);
    return n;
}

QString Composer::previewOf(const QImage &img, int max)
{
    if (img.isNull())
        return {};
    for (int side : { 32, 24, 16 }) { // small enough to send, enough to show blurred
        QImage small = img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
        for (int q = 60; q >= 10; q -= 10) {
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            small.save(&buf, "JPEG", q);
            if (!bytes.isEmpty() && bytes.size() <= max)
                return QString::fromLatin1(bytes.toBase64());
        }
    }
    return {};
}

// ---- adding ----

void Composer::add(const QVariant &urls)
{
    QList<QUrl> list;
    if (urls.canConvert<QList<QUrl>>())
        list = urls.value<QList<QUrl>>();
    if (list.isEmpty())
        for (const QVariant &v : urls.toList())
            list << v.toUrl();
    if (list.isEmpty() && urls.canConvert<QUrl>())
        list << urls.toUrl();
    for (const QUrl &u : list)
        if (u.isLocalFile())
            addFile(u.toLocalFile());
}

bool Composer::canPaste() const
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return false;
    if (md->hasImage())
        return true;
    for (const QUrl &u : md->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile())
            return true;
    return false;
}

bool Composer::paste()
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return false;
    bool any = false;
    for (const QUrl &u : md->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile()) {
            addFile(u.toLocalFile());
            any = true;
        }
    if (!any && md->hasImage()) { // a picture copied from somewhere: kept as a PNG until it is sent
        QImage img = qvariant_cast<QImage>(md->imageData());
        QDir().mkpath(tempDir_);
        const QString path = tempDir_ + QStringLiteral("/Pasted image %1.png").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH.mm.ss"));
        if (!img.isNull() && img.save(path, "PNG")) {
            addFile(path);
            any = true;
        }
    }
    return any;
}

void Composer::addFile(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.isFile() || !fi.isReadable()) {
        emit refused(tr("%1 cannot be read.").arg(fi.fileName()));
        return;
    }
    if (fi.size() == 0) {
        emit refused(tr("%1 is empty.").arg(fi.fileName()));
        return;
    }
    if (count() >= kMaxFiles) {
        emit refused(tr("A message can carry %n files at most.", "", kMaxFiles));
        return;
    }
    for (const QJsonObject &o : items_.items())
        if (extra_.value(o.value("key").toString()).value("path").toString() == fi.absoluteFilePath())
            return; // there already
    QString mime = QMimeDatabase().mimeTypeForFile(fi).name();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+$")).match(mime).hasMatch() || mime.size() > 127)
        mime = QStringLiteral("application/octet-stream");
    const QString key = QString::number(++next_);
    const QString kind = mime.startsWith(QLatin1String("image/")) ? QStringLiteral("image") : mime.startsWith(QLatin1String("video/")) ? QStringLiteral("video") : QStringLiteral("file");
    QJsonObject o{ { "key", key }, { "name", safeName(fi.fileName()) }, { "size", double(fi.size()) }, { "mime", mime }, { "kind", kind },
                   { "url", QUrl::fromLocalFile(fi.absoluteFilePath()).toString() }, { "thumb", "" }, { "state", "preparing" }, { "progress", 0.0 } };
    extra_.insert(key, QJsonObject{ { "path", fi.absoluteFilePath() } });
    QList<QJsonObject> rows = items_.items();
    rows << o;
    items_.setItems(rows);
    emit changed();
    look(key, fi.absoluteFilePath(), mime);
}

// ---- looking at each one ----

void Composer::look(const QString &key, const QString &path, const QString &mime)
{
    if (mime.startsWith(QLatin1String("image/"))) {
        QImageReader reader(path);
        reader.setAutoTransform(true); // a phone photo taken sideways is shown the right way up
        QSize size = reader.size();
        if (size.isValid()) {
            if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                size.transpose();
            reader.setScaledSize(size.scaled(64, 64, Qt::KeepAspectRatio));
        }
        QImage small = reader.read();
        QJsonObject x = extra_.value(key);
        if (size.isValid() && !small.isNull()) {
            x.insert("w", qMin(size.width(), 100000));
            x.insert("h", qMin(size.height(), 100000));
            x.insert("preview", previewOf(small));
        } else {
            update(key, { { "kind", "file" } }); // says it is a picture, but nothing here can read it
        }
        extra_.insert(key, x);
        update(key, { { "state", "ready" } });
        return;
    }
    if (mime.startsWith(QLatin1String("video/")))
        return lookAtVideo(key, path);
    update(key, { { "state", "ready" } });
}

void Composer::lookAtVideo(const QString &key, const QString &path)
{
    // Its first frame, size and length, from Qt Multimedia. A video that cannot be read in time is sent as it is.
    auto *player = new QMediaPlayer();
    auto *sink = new QVideoSink(player);
    player->setVideoOutput(sink);
    players_.insert(key, player);
    auto finish = [this, key, player](const QImage &frame) {
        if (players_.value(key) != player)
            return; // removed meanwhile, or done already
        players_.remove(key);
        QJsonObject x = extra_.value(key);
        if (player->duration() > 0)
            x.insert("duration_ms", double(player->duration()));
        QJsonObject shown{ { "state", "ready" } };
        if (!frame.isNull()) {
            x.insert("w", qMin(frame.width(), 100000));
            x.insert("h", qMin(frame.height(), 100000));
            x.insert("preview", previewOf(frame));
            QDir().mkpath(tempDir_);
            const QString thumb = tempDir_ + QStringLiteral("/frame-%1-%2.jpg").arg(QCoreApplication::applicationPid()).arg(key);
            if (frame.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(thumb, "JPEG", 80))
                shown.insert("thumb", QUrl::fromLocalFile(thumb).toString());
        }
        extra_.insert(key, x);
        if (!item(key).isEmpty())
            update(key, shown);
        player->stop();
        player->deleteLater();
    };
    connect(sink, &QVideoSink::videoFrameChanged, this, [finish](const QVideoFrame &f) {
        if (f.isValid())
            finish(f.toImage());
    });
    connect(player, &QMediaPlayer::errorOccurred, this, [finish] { finish(QImage()); });
    QTimer::singleShot(8000, this, [finish] { finish(QImage()); });
    player->setSource(QUrl::fromLocalFile(path));
    player->play(); // until the first frame shows; there is no audio output, so nothing is heard
}

void Composer::remove(const QString &key)
{
    if (sending_)
        return;
    if (QMediaPlayer *p = players_.take(key)) {
        p->stop();
        p->deleteLater();
    }
    extra_.remove(key);
    QList<QJsonObject> rows;
    for (const QJsonObject &o : items_.items())
        if (o.value("key").toString() != key)
            rows << o;
    items_.setItems(rows);
    emit changed();
}

void Composer::clear()
{
    for (QMediaPlayer *p : players_) {
        p->stop();
        p->deleteLater();
    }
    players_.clear();
    extra_.clear();
    items_.clear();
    emit changed();
}

bool Composer::send(const QString &text, const QString &replyTo)
{
    if (!chats_ || sending_ || !ready())
        return false;
    QVariantList files;
    QList<QJsonObject> rows = items_.items();
    for (QJsonObject &o : rows) {
        const QString key = o.value("key").toString();
        QVariantMap f = extra_.value(key).toVariantMap();
        f.insert("key", key);
        f.insert("name", o.value("name").toString());
        f.insert("mime", o.value("mime").toString());
        files << f;
        o.insert("state", "uploading");
        o.insert("progress", 0.0);
    }
    items_.setItems(rows);
    sending_ = true;
    emit changed();
    chats_->sendFiles(text, replyTo, files);
    return true;
}
