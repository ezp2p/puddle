#pragma once

// The sound of a call (docs/calls.md, "Media"): the microphone, Opus-encoded into 20 ms frames for the Puddle to send, and the
// friend's frames, held in a jitter buffer, decoded and played. The Puddle carries the frames (federation/calls.h); this is
// only the window's end. Without libopus (PUDDLE_HAVE_OPUS unset) a call still connects, in silence.

#include <QByteArray>
#include <QIODevice>
#include <QObject>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

class QAudioSink;
class QAudioSource;

// The friend's frames by RTP timestamp, released in order, 20 ms at a time. It waits until it holds `target` frames before it
// starts (and again whenever it runs dry), says when a frame is missing so the decoder can cover it, drops what comes too late,
// and skips ahead when it falls too far behind. Safe from several threads: frames arrive on a network thread.
class JitterBuffer {
public:
    static constexpr uint32_t kFrameTicks = 960; // 20 ms at 48 kHz
    static constexpr int kMaxFrames = 25;        // 500 ms: more than this behind and it skips ahead

    explicit JitterBuffer(int target = 3) : target_(target) {}

    void push(uint32_t rtpTs, QByteArray frame);
    enum Kind { Frame, Lost, Wait };
    // What to play next: Frame (in `out`), Lost (cover it), or Wait (nothing to play yet: silence)
    Kind pop(QByteArray *out);
    void reset();
    int depth() const;

private:
    int64_t unwrap(uint32_t ts);
    mutable std::mutex mu_;
    std::map<int64_t, QByteArray> frames_;
    int target_;
    bool playing_ = false, started_ = false;
    int64_t next_ = 0;
    uint32_t lastTs_ = 0;
    int64_t lastUnwrapped_ = 0;
};

// What the speaker reads: decoded audio from the jitter buffer, silence when there is none (a sink that reads nothing stops).
class Playout : public QIODevice {
public:
    Playout(JitterBuffer *buffer, int channels, QObject *parent = nullptr);
    ~Playout() override;
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
    qint64 bytesAvailable() const override { return 48000 * channels_ * 2 + QIODevice::bytesAvailable(); }
    bool isSequential() const override { return true; }

private:
    void decodeNext();
    JitterBuffer *buffer_;
    int channels_;
    QByteArray pcm_; // decoded, Int16, interleaved for `channels_`
    void *decoder_ = nullptr;
};

class CallAudio : public QObject {
    Q_OBJECT
public:
    using Send = std::function<void(const QByteArray &opusFrame)>;
    explicit CallAudio(QObject *parent = nullptr);
    ~CallAudio() override;

    static bool built(); // with libopus: sound at all

    // Open the microphone and the speaker for a call; `send` takes each encoded frame. Either device may be missing (none on
    // this computer); the other still works. PUDDLE_TEST_NO_AUDIO in the environment opens neither (the tests).
    void start(Send send);
    void stop();
    bool running() const { return running_; }
    // A frame from the friend: any thread.
    void received(uint32_t rtpTs, const QByteArray &frame) { buffer_.push(rtpTs, frame); }
    void setMuted(bool muted) { muted_ = muted; }
    bool hasInput() const { return source_ != nullptr; }
    bool hasOutput() const { return sink_ != nullptr; }

    // For tests: encode 20 ms of mono 48 kHz Int16 (960 samples) as the microphone would, and decode a frame.
    static QByteArray encodeForTest(const QByteArray &pcm);
    static QByteArray decodeForTest(const QByteArray &frame);

private:
    void readMicrophone();
    JitterBuffer buffer_;
    Send send_;
    QAudioSource *source_ = nullptr;
    QAudioSink *sink_ = nullptr;
    QIODevice *mic_ = nullptr;
    Playout *playout_ = nullptr;
    QByteArray captured_; // mono Int16 at 48 kHz, waiting to make a frame
    int micChannels_ = 1, micRate_ = 48000;
    bool micFloat_ = false;
    double resampleAt_ = 0;
    void *encoder_ = nullptr;
    bool running_ = false, muted_ = false;
};
