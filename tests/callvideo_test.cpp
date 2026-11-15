// A call's video without a camera (src/gui/app/callvideo.h): a picture to I420, H.264 there and back with OpenH264, a decoder
// that comes in late waiting for the next key frame, and a call's senders coming and going, shown in the sinks QML hands it.
#include "callvideo.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QVideoSink>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

template <typename F> static bool until(F f, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!f()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

// a picture that moves: a gradient, and a square that crosses it
static QImage scene(int k)
{
    QImage img(800, 600, QImage::Format_RGB32);
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++)
            img.setPixel(x, y, qRgb(x * 255 / 800, y * 255 / 600, 128));
    QPainter p(&img);
    p.fillRect(100 + k * 10, 200, 120, 120, Qt::white);
    return img;
}

static double lumaError(const Picture &a, const Picture &b)
{
    if (a.width != b.width || a.height != b.height || a.y.size() != b.y.size())
        return 1e9;
    double sum = 0;
    for (qsizetype i = 0; i < a.y.size(); i++)
        sum += std::abs(int(uint8_t(a.y[i])) - int(uint8_t(b.y[i])));
    return sum / double(a.y.size());
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    /* ---- a picture to I420: scaled to cover, the middle kept; red is red ---- */
    {
        QImage red(1280, 720, QImage::Format_RGB32);
        red.fill(qRgb(255, 0, 0));
        const Picture p = pictureOf(red, 640, 480);
        CHECK(p.width == 640 && p.height == 480 && p.y.size() == 640 * 480 && p.u.size() == 320 * 240);
        CHECK(std::abs(uint8_t(p.y[1000]) - 82) <= 2 && std::abs(uint8_t(p.u[1000]) - 90) <= 2 && std::abs(uint8_t(p.v[1000]) - 240) <= 2);
        const QVideoFrame f = p.toFrame();
        CHECK(f.isValid() && f.size() == QSize(640, 480) && f.pixelFormat() == QVideoFrameFormat::Format_YUV420P);
    }

    if (!CallVideo::isBuilt()) {
        printf("callvideo_test: ok (built without OpenH264: video not tested)\n");
        return failures ? 1 : 0;
    }

    /* ---- H.264 there and back; a decoder that comes in late sees nothing until the next key frame ---- */
    {
        H264Encoder enc;
        CHECK(enc.open(CallVideo::kWidth, CallVideo::kHeight, CallVideo::kFps, CallVideo::kBitrate));
        H264Decoder dec, late;
        QList<QByteArray> aus;
        Picture last, lastIn;
        int firstLate = -1;
        for (int k = 0; k < 36; k++) {
            lastIn = pictureOf(scene(k), CallVideo::kWidth, CallVideo::kHeight);
            const QByteArray au = enc.encode(lastIn, k * 66);
            aus.append(au);
            if (au.isEmpty())
                continue;
            const Picture out = dec.decode(au);
            if (!out.isNull())
                last = out;
            if (k >= 5 && firstLate < 0 && !late.decode(au).isNull())
                firstLate = k;
        }
        CHECK(aus.value(0).startsWith(QByteArray("\x00\x00\x00\x01\x67", 5)));      /* a key frame first: SPS ... */
        qsizetype total = 0;
        for (const QByteArray &a : aus)
            total += a.size();
        CHECK(total > 0 && total < 36 * 60000);                                     /* about what 600 kbit/s allows */
        CHECK(last.width == 640 && last.height == 480);
        CHECK(lumaError(last, lastIn) < 8);                                         /* what came out looks like what went in */
        CHECK(firstLate >= 30);                                                     /* the next key frame: 2 s at 15 a second */
    }

    /* ---- a call: the camera's pictures go out encoded; a sender's come in, decoded, to the sink attached for them ---- */
    {
        CallVideo v;
        QList<QByteArray> sent;
        v.start([&](const QByteArray &au) { QMetaObject::invokeMethod(&app, [&sent, au] { sent.append(au); }); });
        CHECK(v.running() && !v.cameraOn());
        v.feed(scene(0));
        until([] { return false; }, 200);
        CHECK(sent.isEmpty());                                                      /* the camera is off */
        v.setCamera(true);
        CHECK(v.cameraOn());
        for (int k = 0; k < 10; k++) {
            v.feed(scene(k));
            until([] { return false; }, 66);
        }
        CHECK(until([&] { return sent.size() >= 8; }));

        QVideoSink sink;
        int frames = 0;
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, [&](const QVideoFrame &f) { frames += f.isValid(); });
        for (const QByteArray &au : sent)
            v.received(QStringLiteral("bob"), 0, au);
        CHECK(until([&] { return v.sources() == QStringList{ "bob" }; }));
        v.attach(QStringLiteral("bob"), &sink);
        CHECK(until([&] { return frames >= 1; }));                                  /* the last picture, at once */
        v.received(QStringLiteral("bob"), 0, sent.last());
        CHECK(until([&] { return frames >= 2; }));
        CHECK(until([&] { return v.sources().isEmpty(); }, 4000));                  /* nothing more from bob: gone */
        v.stop();
        CHECK(!v.running() && !v.cameraOn());
    }

    if (failures) {
        fprintf(stderr, "callvideo_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("callvideo_test: ok\n");
    return 0;
}
