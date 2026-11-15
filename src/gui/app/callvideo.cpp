#include "callvideo.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCoreApplication>
#include <QDateTime>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoFrameFormat>
#include <QVideoSink>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif

#include <algorithm>
#include <cstring>

#ifdef PUDDLE_HAVE_OPENH264
#include <wels/codec_api.h>
#endif

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// ---- pictures ----

QVideoFrame Picture::toFrame() const
{
    if (isNull())
        return {};
    QVideoFrame f(QVideoFrameFormat(QSize(width, height), QVideoFrameFormat::Format_YUV420P));
    if (!f.map(QVideoFrame::WriteOnly))
        return {};
    const QByteArray *planes[3] = { &y, &u, &v };
    for (int p = 0; p < 3; p++) {
        const int w = p ? width / 2 : width, h = p ? height / 2 : height;
        for (int row = 0; row < h; row++)
            memcpy(f.bits(p) + qsizetype(row) * f.bytesPerLine(p), planes[p]->constData() + qsizetype(row) * w, size_t(w));
    }
    f.unmap();
    return f;
}

Picture pictureOf(const QImage &image, int w, int h)
{
    Picture pic;
    if (image.isNull() || w <= 0 || h <= 0)
        return pic;
    QImage img = image.convertToFormat(QImage::Format_RGB32).scaled(w, h, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    img = img.copy((img.width() - w) / 2, (img.height() - h) / 2, w, h);
    pic.width = w, pic.height = h;
    pic.y.resize(qsizetype(w) * h);
    pic.u.resize(qsizetype(w / 2) * (h / 2));
    pic.v.resize(pic.u.size());
    auto *Y = reinterpret_cast<uint8_t *>(pic.y.data());
    auto *U = reinterpret_cast<uint8_t *>(pic.u.data());
    auto *V = reinterpret_cast<uint8_t *>(pic.v.data());
    for (int row = 0; row < h; row++) { // BT.601, studio range
        const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(row));
        for (int x = 0; x < w; x++) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            Y[row * w + x] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int row = 0; row < h / 2; row++) {
        const auto *a = reinterpret_cast<const QRgb *>(img.constScanLine(row * 2));
        const auto *b2 = reinterpret_cast<const QRgb *>(img.constScanLine(row * 2 + 1));
        for (int x = 0; x < w / 2; x++) {
            const QRgb p[4] = { a[2 * x], a[2 * x + 1], b2[2 * x], b2[2 * x + 1] };
            int r = 0, g = 0, b = 0;
            for (QRgb c : p)
                r += qRed(c), g += qGreen(c), b += qBlue(c);
            r /= 4, g /= 4, b /= 4;
            U[row * (w / 2) + x] = uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            V[row * (w / 2) + x] = uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
    return pic;
}

// ---- H.264 ----

#ifdef PUDDLE_HAVE_OPENH264

H264Encoder::~H264Encoder()
{
    if (auto *e = static_cast<ISVCEncoder *>(enc_)) {
        e->Uninitialize();
        WelsDestroySVCEncoder(e);
    }
}

bool H264Encoder::open(int width, int height, int fps, int bitrate)
{
    ISVCEncoder *e = nullptr;
    if (WelsCreateSVCEncoder(&e) != 0 || !e)
        return false;
    SEncParamExt p;
    e->GetDefaultParams(&p);
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = width;
    p.iPicHeight = height;
    p.fMaxFrameRate = float(fps);
    p.iTargetBitrate = bitrate;
    p.iRCMode = RC_BITRATE_MODE;
    p.bEnableFrameSkip = true;
    p.uiIntraPeriod = uint32_t(fps * 2); // a key frame every 2 s: whoever joins, or lost some, sees again soon
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.iMultipleThreadIdc = 1;
    p.iEntropyCodingModeFlag = 0; // CAVLC: Constrained Baseline
    p.sSpatialLayers[0].iVideoWidth = width;
    p.sSpatialLayers[0].iVideoHeight = height;
    p.sSpatialLayers[0].fFrameRate = float(fps);
    p.sSpatialLayers[0].iSpatialBitrate = bitrate;
    p.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
    if (e->InitializeExt(&p) != 0) {
        WelsDestroySVCEncoder(e);
        return false;
    }
    int fmt = videoFormatI420;
    e->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);
    enc_ = e;
    width_ = width, height_ = height;
    return true;
}

QByteArray H264Encoder::encode(const Picture &pic, qint64 ms)
{
    auto *e = static_cast<ISVCEncoder *>(enc_);
    if (!e || pic.width != width_ || pic.height != height_)
        return {};
    SSourcePicture src;
    memset(&src, 0, sizeof src);
    src.iColorFormat = videoFormatI420;
    src.iPicWidth = width_;
    src.iPicHeight = height_;
    src.iStride[0] = width_;
    src.iStride[1] = src.iStride[2] = width_ / 2;
    src.pData[0] = reinterpret_cast<unsigned char *>(const_cast<char *>(pic.y.constData()));
    src.pData[1] = reinterpret_cast<unsigned char *>(const_cast<char *>(pic.u.constData()));
    src.pData[2] = reinterpret_cast<unsigned char *>(const_cast<char *>(pic.v.constData()));
    src.uiTimeStamp = ms;
    SFrameBSInfo info;
    memset(&info, 0, sizeof info);
    if (e->EncodeFrame(&src, &info) != cmResultSuccess || info.eFrameType == videoFrameTypeSkip)
        return {};
    QByteArray out;
    for (int i = 0; i < info.iLayerNum; i++) {
        const SLayerBSInfo &l = info.sLayerInfo[i];
        int size = 0;
        for (int n = 0; n < l.iNalCount; n++)
            size += l.pNalLengthInByte[n];
        out.append(reinterpret_cast<const char *>(l.pBsBuf), size);
    }
    return out;
}

void H264Encoder::keyframe()
{
    if (auto *e = static_cast<ISVCEncoder *>(enc_))
        e->ForceIntraFrame(true);
}

H264Decoder::H264Decoder()
{
    ISVCDecoder *d = nullptr;
    if (WelsCreateDecoder(&d) != 0 || !d)
        return;
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    p.eEcActiveIdc = ERROR_CON_DISABLE; // a broken picture is not shown, rather than shown smeared
    if (d->Initialize(&p) != 0) {
        WelsDestroyDecoder(d);
        return;
    }
    dec_ = d;
}

H264Decoder::~H264Decoder()
{
    if (auto *d = static_cast<ISVCDecoder *>(dec_)) {
        d->Uninitialize();
        WelsDestroyDecoder(d);
    }
}

Picture H264Decoder::decode(const QByteArray &au)
{
    Picture pic;
    auto *d = static_cast<ISVCDecoder *>(dec_);
    if (!d || au.isEmpty())
        return pic;
    unsigned char *dst[3] = {};
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    d->DecodeFrameNoDelay(reinterpret_cast<const unsigned char *>(au.constData()), int(au.size()), dst, &bi);
    if (bi.iBufferStatus != 1 || !dst[0])
        return pic;
    const int w = bi.UsrData.sSystemBuffer.iWidth, h = bi.UsrData.sSystemBuffer.iHeight;
    const int sy = bi.UsrData.sSystemBuffer.iStride[0], suv = bi.UsrData.sSystemBuffer.iStride[1];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return pic;
    pic.width = w & ~1, pic.height = h & ~1;
    pic.y.resize(qsizetype(pic.width) * pic.height);
    pic.u.resize(qsizetype(pic.width / 2) * (pic.height / 2));
    pic.v.resize(pic.u.size());
    for (int row = 0; row < pic.height; row++)
        memcpy(pic.y.data() + qsizetype(row) * pic.width, dst[0] + qsizetype(row) * sy, size_t(pic.width));
    for (int row = 0; row < pic.height / 2; row++) {
        memcpy(pic.u.data() + qsizetype(row) * (pic.width / 2), dst[1] + qsizetype(row) * suv, size_t(pic.width / 2));
        memcpy(pic.v.data() + qsizetype(row) * (pic.width / 2), dst[2] + qsizetype(row) * suv, size_t(pic.width / 2));
    }
    return pic;
}

bool CallVideo::isBuilt() { return true; }

#else // without OpenH264: no video

H264Encoder::~H264Encoder() = default;
bool H264Encoder::open(int, int, int, int) { return false; }
QByteArray H264Encoder::encode(const Picture &, qint64) { return {}; }
void H264Encoder::keyframe() {}
H264Decoder::H264Decoder() = default;
H264Decoder::~H264Decoder() = default;
Picture H264Decoder::decode(const QByteArray &) { return {}; }
bool CallVideo::isBuilt() { return false; }

#endif

// ---- the call's video ----

CallVideo::CallVideo(QObject *parent) : QObject(parent)
{
    worker_ = std::thread([this] { work(); });
    sweep_.setInterval(500);
    connect(&sweep_, &QTimer::timeout, this, &CallVideo::sweep);
}

CallVideo::~CallVideo()
{
    stop();
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    worker_.join();
}

void CallVideo::post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_)
            return;
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void CallVideo::work()
{
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        wake_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
        if (quit_)
            break;
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        lk.unlock();
        job();
        lk.lock();
    }
    lk.unlock();
    encoder_.reset();
    decoders_.clear();
}

void CallVideo::start(Send send)
{
    {
        std::lock_guard<std::mutex> g(mu_);
        send_ = std::move(send);
    }
    if (!running_) {
        running_ = true;
        sweep_.start();
        emit runningChanged();
    }
}

void CallVideo::stop()
{
    setCamera(false);
    {
        std::lock_guard<std::mutex> g(mu_);
        send_ = nullptr;
    }
    post([this] { // the next call starts afresh
        encoder_.reset();
        decoders_.clear();
    });
    sweep_.stop();
    const bool had = !sources_.isEmpty();
    sources_.clear();
    if (had)
        emit sourcesChanged();
    if (running_) {
        running_ = false;
        emit runningChanged();
    }
}

void CallVideo::setCamera(bool on)
{
    on = on && built() && running_;
    if (on == cameraOn_)
        return;
    cameraOn_ = on;
    if (on) {
        if (!qEnvironmentVariableIsSet("PUDDLE_TEST_NO_AUDIO"))
            openCamera();
        post([this] {
            if (encoder_)
                encoder_->keyframe(); /* the others see it at once */
        });
    } else {
        closeCamera();
        if (self_)
            self_->setVideoFrame(QVideoFrame());
    }
    emit cameraChanged();
}

void CallVideo::openCamera()
{
    if (camera_)
        return;
    const QCameraDevice dev = QMediaDevices::defaultVideoInput();
    if (dev.isNull()) {
        emit cameraFailed(tr("There is no camera."));
        return;
    }
#if QT_CONFIG(permissions)
    if (QCoreApplication::instance()) {
        QCameraPermission perm;
        switch (qApp->checkPermission(perm)) {
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(perm, this, [this](const QPermission &p) {
                if (p.status() == Qt::PermissionStatus::Granted && cameraOn_)
                    openCamera();
                else if (p.status() != Qt::PermissionStatus::Granted)
                    emit cameraFailed(tr("Puddle may not use the camera (see the system's privacy settings)."));
            });
            return;
        case Qt::PermissionStatus::Denied:
            emit cameraFailed(tr("Puddle may not use the camera (see the system's privacy settings)."));
            return;
        default:
            break;
        }
    }
#endif
    camera_ = new QCamera(dev, this);
    // the smallest format at least as big as what is sent, at 15 frames a second or more: less to scale every frame
    QCameraFormat best;
    for (const QCameraFormat &f : dev.videoFormats()) {
        const QSize s = f.resolution();
        if (s.width() < kWidth || s.height() < kHeight || f.maxFrameRate() < kFps)
            continue;
        if (best.isNull() || qint64(s.width()) * s.height() < qint64(best.resolution().width()) * best.resolution().height())
            best = f;
    }
    if (!best.isNull())
        camera_->setCameraFormat(best);
    session_ = new QMediaCaptureSession(this);
    camSink_ = new QVideoSink(this);
    session_->setCamera(camera_);
    session_->setVideoSink(camSink_);
    connect(camSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
        if (self_)
            self_->setVideoFrame(f);
        const qint64 now = nowMs();
        if (now - lastShot_ < 1000 / kFps - 5)
            return;
        lastShot_ = now;
        encodeLater(f.toImage());
    });
    connect(camera_, &QCamera::errorOccurred, this, [this](QCamera::Error, const QString &why) { emit cameraFailed(why); });
    camera_->start();
}

void CallVideo::closeCamera()
{
    if (camera_)
        camera_->stop();
    delete session_;
    delete camSink_;
    delete camera_;
    session_ = nullptr, camSink_ = nullptr, camera_ = nullptr;
}

void CallVideo::feed(const QImage &image)
{
    if (!cameraOn_)
        return;
    if (self_)
        self_->setVideoFrame(pictureOf(image, kWidth, kHeight).toFrame());
    encodeLater(image);
}

void CallVideo::encodeLater(const QImage &image)
{
    if (image.isNull())
        return;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (pending_ > 1)
            return; /* falling behind: this one is dropped, the next is newer anyway */
        pending_++;
    }
    post([this, image] {
        const Picture pic = pictureOf(image, kWidth, kHeight);
        if (!encoder_) {
            encoder_ = std::make_unique<H264Encoder>();
            if (!encoder_->open(kWidth, kHeight, kFps, kBitrate))
                encoder_.reset();
        }
        const QByteArray au = encoder_ ? encoder_->encode(pic, nowMs()) : QByteArray();
        Send send;
        {
            std::lock_guard<std::mutex> g(mu_);
            pending_--;
            send = send_;
        }
        if (!au.isEmpty() && send)
            send(au);
    });
}

void CallVideo::received(const QString &from, uint32_t, const QByteArray &au)
{
    if (!built())
        return;
    post([this, from, au] {
        auto &d = decoders_[from];
        if (!d)
            d = std::make_shared<H264Decoder>();
        const Picture pic = d->decode(au);
        if (!pic.isNull())
            QMetaObject::invokeMethod(this, [this, from, pic] { deliver(from, pic); }, Qt::QueuedConnection);
    });
}

void CallVideo::deliver(const QString &key, const Picture &picture)
{
    if (!running_)
        return;
    Source &s = sources_[key];
    const bool fresh = s.last == 0;
    s.last = nowMs();
    s.frame = picture.toFrame();
    if (s.sink)
        s.sink->setVideoFrame(s.frame);
    if (fresh)
        emit sourcesChanged();
}

void CallVideo::sweep()
{
    const qint64 now = nowMs();
    QStringList gone;
    for (auto it = sources_.begin(); it != sources_.end();)
        if (now - it->last > kGoneMs) {
            gone.append(it.key());
            it = sources_.erase(it);
        } else {
            ++it;
        }
    if (gone.isEmpty())
        return;
    post([this, gone] { /* when they come back, they start from a key frame */
        for (const QString &k : gone)
            decoders_.remove(k);
    });
    emit sourcesChanged();
}

QStringList CallVideo::sources() const
{
    QStringList keys = sources_.keys();
    keys.sort();
    return keys;
}

void CallVideo::attach(const QString &key, QObject *videoSink)
{
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    if (key == QLatin1String("self")) {
        self_ = sink;
        return;
    }
    auto it = sources_.find(key);
    if (it == sources_.end())
        return;
    it->sink = sink;
    if (sink && it->frame.isValid())
        sink->setVideoFrame(it->frame);
}
