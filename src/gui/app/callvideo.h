#pragma once

// Video in a call (docs/calls.md, "Video"): this end's camera, encoded as H.264 (OpenH264) for the Puddle to send, and each
// sender's frames decoded and shown. The Puddle carries the frames and decodes nothing (federation/calls.h); this is only the
// device's end, the window's or a phone's. Without OpenH264 (PUDDLE_HAVE_OPENH264 unset) there is no video: calls are voice.
//
// Encoding and decoding run on a thread of their own, never the window's. QML shows a sender with a VideoOutput whose sink it
// hands to attach(): "self" for this end's camera, or the key a sender's frames came with (a friend's or member's id; on a
// phone, "" for the friend and "slot:<n>" for a member, as the leg says).

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVideoFrame>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class QCamera;
class QMediaCaptureSession;
class QScreenCapture;
class QVideoSink;

// A decoded picture, I420 (Y, then U and V at half size each way), tightly packed.
struct Picture {
    int width = 0, height = 0;
    QByteArray y, u, v;
    bool isNull() const { return width <= 0; }
    QVideoFrame toFrame() const;
};

// I420 at `w`x`h` from any image: scaled to cover, the middle kept (a camera's picture).
Picture pictureOf(const QImage &image, int w, int h);
// I420 at `w`x`h` from any image: scaled to fit, all of it kept, black around it (a screen: nothing may be cut off).
Picture pictureFitted(const QImage &image, int w, int h);

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();
    H264Encoder(const H264Encoder &) = delete;
    H264Encoder &operator=(const H264Encoder &) = delete;
    bool open(int width, int height, int fps, int bitrate);
    // An access unit, Annex B (start codes); empty when the encoder skipped this one (to keep to the bitrate) or failed.
    QByteArray encode(const Picture &picture, qint64 ms);
    void keyframe(); // the next is a key frame
private:
    void *enc_ = nullptr;
    int width_ = 0, height_ = 0;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;
    // The picture in this access unit; null if there is none yet (waiting for a key frame) or it was broken.
    Picture decode(const QByteArray &au);
private:
    void *dec_ = nullptr;
};

class CallVideo : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool built READ built CONSTANT)                    // with OpenH264: video at all
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)   // in a call with sound here
    Q_PROPERTY(bool cameraOn READ cameraOn NOTIFY cameraChanged)  // this end is sending
    Q_PROPERTY(bool screenOn READ screenOn NOTIFY screenChanged)  // this end shares its screen
    Q_PROPERTY(bool canShareScreen READ canShareScreen NOTIFY runningChanged) // this call can carry a shared screen (a phone's can't yet)
    // those whose video is coming in now, in the order they came (so nobody moves when someone else comes or goes): a sender's
    // camera by their key, their screen as "<key>/screen"
    Q_PROPERTY(QStringList sources READ sources NOTIFY sourcesChanged)
public:
    using Send = std::function<void(const QByteArray &au)>;
    static constexpr int kWidth = 640, kHeight = 480, kFps = 15, kBitrate = 600000;
    // a shared screen: bigger, fewer frames, more bits (text must stay sharp)
    static constexpr int kScreenWidth = 1280, kScreenHeight = 720, kScreenFps = 10, kScreenBitrate = 2500000;
    static constexpr int kGoneMs = 2500; // a sender with no frame for this long has stopped

    explicit CallVideo(QObject *parent = nullptr);
    ~CallVideo() override;

    static bool isBuilt();
    bool built() const { return isBuilt(); }

    // In a call: `send` takes each access unit of the camera, `sendScreen` of the shared screen (none: this call can't carry
    // one). stop() closes the camera and the screen, and forgets every sender.
    void start(Send send, Send sendScreen = nullptr);
    void stop();
    bool running() const { return running_; }
    // The camera on or off. PUDDLE_TEST_NO_AUDIO in the environment never opens one (the tests feed() instead).
    void setCamera(bool on);
    bool cameraOn() const { return cameraOn_; }
    // Share the screen, or stop. Asks the system which screen (and for leave) where it must. PUDDLE_TEST_NO_AUDIO: never really.
    void setScreen(bool on);
    bool screenOn() const { return screenOn_; }
    bool canShareScreen() const { return running_ && sendScreen_ != nullptr; }
    // A sender's access unit: any thread.
    void received(const QString &from, uint32_t ts, const QByteArray &au);
    QStringList sources() const;
    Q_INVOKABLE void attach(const QString &key, QObject *videoSink);
    // A picture as if the camera took it, or the screen (tests).
    void feed(const QImage &image);
    void feedScreen(const QImage &image);

signals:
    void runningChanged();
    void cameraChanged();
    void screenChanged();
    void sourcesChanged();
    void cameraFailed(const QString &why);

private:
    void post(std::function<void()> job);
    void work();
    void encodeLater(const QImage &image);
    void encodeScreenLater(const QImage &image);
    void openScreen();
    void closeScreen();
    void deliver(const QString &key, const Picture &picture);
    void openCamera();
    void closeCamera();
    void sweep();

    // the worker's
    std::mutex mu_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    std::thread worker_;
    std::unique_ptr<H264Encoder> encoder_;               // worker only
    std::unique_ptr<H264Encoder> screenEncoder_;         // worker only
    QHash<QString, std::shared_ptr<H264Decoder>> decoders_; // worker only
    Send send_, sendScreen_;                             // set under mu_
    int pending_ = 0, screenPending_ = 0;                // pictures waiting to be encoded (under mu_): more than one, drop

    // the window's thread's
    bool running_ = false, cameraOn_ = false, screenOn_ = false;
    QScreenCapture *screen_ = nullptr;
    QMediaCaptureSession *screenSession_ = nullptr;
    QVideoSink *screenSink_ = nullptr;
    qint64 lastScreenShot_ = 0;
    QPointer<QVideoSink> selfScreen_;
    QStringList order_;                                  // the senders, in the order they came
    QCamera *camera_ = nullptr;
    QMediaCaptureSession *session_ = nullptr;
    QVideoSink *camSink_ = nullptr;
    qint64 lastShot_ = 0;
    struct Source {
        QPointer<QVideoSink> sink;
        qint64 last = 0;
        QVideoFrame frame; // the last one, for a sink attached after it came
    };
    QHash<QString, Source> sources_;
    QPointer<QVideoSink> self_;
    QTimer sweep_;
};
