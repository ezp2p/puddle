#pragma once

// The sound of a call (docs/calls.md, "Media"): the microphone, Opus-encoded into 20 ms frames for the Puddle to send, and the
// friend's frames, held in a jitter buffer, decoded and played. The Puddle carries the frames (federation/calls.h); this is
// only the window's end. Without libopus (PUDDLE_HAVE_OPUS unset) a call still connects, in silence.

#include <QByteArray>
#include <QIODevice>
#include <QObject>

#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

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

// Takes the speaker's sound back out of the microphone's (docs/calls.md, "Echo"), so a friend on the far end doesn't hear
// themself from this computer's speakers. With webrtc-audio-processing (PUDDLE_HAVE_WEBRTC_APM), WebRTC's own echo canceller
// with its noise suppression and high-pass filter; else with speexdsp (PUDDLE_HAVE_SPEEXDSP) its echo canceller and
// preprocessor; else nothing (built() is false and process() leaves the sound as it is).
//
// The speaker's side tells it each sample it hands the speaker (played(), any thread); the microphone's side passes each
// 10 ms of mono 48 kHz Int16 through process(). The canceller needs, beside each 10 ms from the microphone, what the speaker
// was playing then or a little before: a reference a little ahead of the echo, never behind it. Nothing says exactly where a
// speaker is (Qt's own measure of its queue moves in steps), but it can't be past what it was handed, and a speaker takes
// its sound in gulps (170 ms at a time on PipeWire, a few ms on others) just before it runs out, so the handed count rises
// in stairs whose foot is where the speaker is. The reference advances 10 ms a chunk, steadily; when it would pass what
// was handed it comes back to it (so it runs along the stairs' foot), and when it has stayed more than 20 ms below them for
// 3 s (the two clocks drifting apart) it moves up to them.
class EchoCanceller {
public:
    static constexpr int kChunk = 480; // 10 ms at 48 kHz

    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller &) = delete;
    EchoCanceller &operator=(const EchoCanceller &) = delete;

    static bool built();
    static const char *kind(); // "webrtc", "speex" or "none"

    void played(const int16_t *mono, int samples);
    int64_t playedCount() const;
    // `pcm` holds a whole number of 10 ms chunks, just recorded; changed in place
    void process(int16_t *pcm, int samples);
    int resyncs() const { return resyncs_; } // times the reference was moved more than 20 ms (for tests)

private:
    void reference(int16_t *out, int64_t end);
    struct Impl;
    std::unique_ptr<Impl> d_;
    mutable std::mutex mu_;
    std::vector<int16_t> ring_; // the last second the speaker was handed
    int64_t count_ = 0;
    static constexpr int kWindow = 300; // chunks: 3 s
    int64_t next_ = -1; // where the next chunk's reference ends
    int64_t low_ = INT64_MAX; // the lowest (handed - next_) this window
    int left_ = kWindow;
    int resyncs_ = 0;
};

// The voices of a call, each with its own jitter buffer and decoder (a friend, or each member of a group call), mixed. A
// voice not heard for 10 s is let go. Frames arrive on network threads; the speaker pulls on its own.
class Mixer {
public:
    Mixer() = default;
    ~Mixer();
    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;

    void push(const QString &from, uint32_t rtpTs, QByteArray frame);
    void pull(int16_t out[960]); // the next 20 ms, mono at 48 kHz: silence where there is nothing
    void reset();
    int voices() const;

private:
    struct Voice {
        JitterBuffer buffer;
        void *decoder = nullptr;
        int quiet = 0; // 20 ms in a row with nothing
    };
    mutable std::mutex mu_;
    std::map<QString, std::unique_ptr<Voice>> voices_;
};

// What the speaker reads: the mix, silence when there is none (a sink that reads nothing stops).
class Playout : public QIODevice {
public:
    Playout(Mixer *mixer, int channels, QObject *parent = nullptr);
    ~Playout() override;
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
    qint64 bytesAvailable() const override { return 48000 * channels_ * 2 + QIODevice::bytesAvailable(); }
    bool isSequential() const override { return true; }
    // Tell `echo` each sample handed to the speaker (null: none). Set before the speaker starts reading.
    void setEcho(EchoCanceller *echo) { echo_ = echo; }

private:
    void decodeNext();
    EchoCanceller *echo_ = nullptr;
    Mixer *mixer_;
    int channels_;
    QByteArray pcm_; // decoded, Int16, interleaved for `channels_`
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
    // A frame from the friend, or from member `from` in a group call: any thread.
    void received(const QString &from, uint32_t rtpTs, const QByteArray &frame) { mixer_.push(from, rtpTs, frame); }
    void setMuted(bool muted) { muted_ = muted; }
    bool hasInput() const { return source_ != nullptr; }
    bool hasOutput() const { return sink_ != nullptr; }
    bool cancelsEcho() const { return echo_ != nullptr; } // both devices open, and built with an echo canceller

    // For tests: encode 20 ms of mono 48 kHz Int16 (960 samples) as the microphone would, and decode a frame.
    static QByteArray encodeForTest(const QByteArray &pcm);
    static QByteArray decodeForTest(const QByteArray &frame);

private:
    void readMicrophone();
    Mixer mixer_;
    Send send_;
    QAudioSource *source_ = nullptr;
    QAudioSink *sink_ = nullptr;
    QIODevice *mic_ = nullptr;
    Playout *playout_ = nullptr;
    EchoCanceller *echo_ = nullptr;
    QByteArray captured_; // mono Int16 at 48 kHz, waiting to make a frame
    int micChannels_ = 1, micRate_ = 48000;
    bool micFloat_ = false;
    double resampleAt_ = 0;
    void *encoder_ = nullptr;
    bool running_ = false, muted_ = false;
};
