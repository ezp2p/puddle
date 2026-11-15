#include "callaudio.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>

#include <cstring>

#ifdef PUDDLE_HAVE_OPUS
#include <opus.h>
#endif

static constexpr int kRate = 48000, kFrameSamples = 960; // 20 ms

// ---- the jitter buffer ----

int64_t JitterBuffer::unwrap(uint32_t ts)
{
    // RTP timestamps wrap at 2^32: carry on from the last one by the (signed) step between them
    if (!started_) {
        started_ = true;
        lastTs_ = ts;
        lastUnwrapped_ = int64_t(1) << 40;
        return lastUnwrapped_;
    }
    int64_t step = int32_t(ts - lastTs_);
    int64_t u = lastUnwrapped_ + step;
    if (step > 0) {
        lastTs_ = ts;
        lastUnwrapped_ = u;
    }
    return u;
}

void JitterBuffer::push(uint32_t rtpTs, QByteArray frame)
{
    std::lock_guard<std::mutex> g(mu_);
    int64_t t = unwrap(rtpTs);
    if (playing_ && t < next_)
        return; // too late: its moment has gone
    frames_[t] = std::move(frame);
    if (int(frames_.size()) > kMaxFrames) { // far behind: keep only the newest `target`
        while (int(frames_.size()) > target_)
            frames_.erase(frames_.begin());
        if (playing_)
            next_ = frames_.begin()->first;
    }
}

JitterBuffer::Kind JitterBuffer::pop(QByteArray *out)
{
    std::lock_guard<std::mutex> g(mu_);
    if (!playing_) {
        if (int(frames_.size()) < target_)
            return Wait;
        playing_ = true;
        next_ = frames_.begin()->first;
    }
    if (frames_.empty()) {
        playing_ = false; // ran dry: fill up again before playing on
        return Wait;
    }
    auto it = frames_.begin();
    if (it->first > next_ + 5 * int64_t(kFrameTicks))
        next_ = it->first; // more than 100 ms missing: carry on from what came, rather than guess at all of it
    if (it->first == next_) {
        *out = std::move(it->second);
        frames_.erase(it);
        next_ += kFrameTicks;
        return Frame;
    }
    next_ += kFrameTicks; // a gap: this one went missing (or comes too late)
    return Lost;
}

void JitterBuffer::reset()
{
    std::lock_guard<std::mutex> g(mu_);
    frames_.clear();
    playing_ = started_ = false;
}

int JitterBuffer::depth() const
{
    std::lock_guard<std::mutex> g(mu_);
    return int(frames_.size());
}

// ---- the speaker's side ----

Playout::Playout(JitterBuffer *buffer, int channels, QObject *parent) : QIODevice(parent), buffer_(buffer), channels_(channels)
{
#ifdef PUDDLE_HAVE_OPUS
    int err = 0;
    decoder_ = opus_decoder_create(kRate, 1, &err);
#endif
}

Playout::~Playout()
{
#ifdef PUDDLE_HAVE_OPUS
    if (decoder_)
        opus_decoder_destroy(static_cast<OpusDecoder *>(decoder_));
#endif
}

void Playout::decodeNext()
{
    int16_t mono[kFrameSamples * 6]; // Opus may hand back up to 120 ms
    int n = 0;
    QByteArray frame;
    JitterBuffer::Kind k = buffer_->pop(&frame);
#ifdef PUDDLE_HAVE_OPUS
    auto *dec = static_cast<OpusDecoder *>(decoder_);
    if (dec && k == JitterBuffer::Frame)
        n = opus_decode(dec, reinterpret_cast<const unsigned char *>(frame.constData()), frame.size(), mono, kFrameSamples * 6, 0);
    else if (dec && k == JitterBuffer::Lost)
        n = opus_decode(dec, nullptr, 0, mono, kFrameSamples, 0); // the decoder's best guess at what was lost
#endif
    if (n <= 0) {
        n = kFrameSamples;
        memset(mono, 0, sizeof(int16_t) * n);
    }
    const qsizetype at = pcm_.size();
    pcm_.resize(at + qsizetype(n) * channels_ * 2);
    auto *out = reinterpret_cast<int16_t *>(pcm_.data() + at);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < channels_; c++)
            out[i * channels_ + c] = mono[i];
}

qint64 Playout::readData(char *data, qint64 maxlen)
{
    const qint64 frame = qint64(channels_) * 2;
    maxlen -= maxlen % frame;
    while (pcm_.size() < maxlen)
        decodeNext();
    memcpy(data, pcm_.constData(), size_t(maxlen));
    pcm_.remove(0, maxlen);
    return maxlen;
}

// ---- the call's sound ----

CallAudio::CallAudio(QObject *parent) : QObject(parent) {}

CallAudio::~CallAudio()
{
    stop();
}

bool CallAudio::built()
{
#ifdef PUDDLE_HAVE_OPUS
    return true;
#else
    return false;
#endif
}

static QAudioFormat wanted(int channels)
{
    QAudioFormat f;
    f.setSampleRate(kRate);
    f.setChannelCount(channels);
    f.setSampleFormat(QAudioFormat::Int16);
    return f;
}

void CallAudio::start(Send send)
{
    stop();
    send_ = std::move(send);
    buffer_.reset();
    captured_.clear();
    resampleAt_ = 0;
    running_ = true;
#ifdef PUDDLE_HAVE_OPUS
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (enc) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    }
    encoder_ = enc;
#endif
    if (qEnvironmentVariableIsSet("PUDDLE_TEST_NO_AUDIO"))
        return; // the tests: never this computer's own microphone and speaker
    // the speaker: 48 kHz Int16, mono if it takes it, else as many channels as it likes (each gets the same)
    const QAudioDevice out = QMediaDevices::defaultAudioOutput();
    if (!out.isNull()) {
        QAudioFormat f = wanted(1);
        if (!out.isFormatSupported(f))
            f = wanted(out.preferredFormat().channelCount() > 0 ? out.preferredFormat().channelCount() : 2);
        sink_ = new QAudioSink(out, f, this);
        sink_->setBufferSize(f.bytesForDuration(80000));
        playout_ = new Playout(&buffer_, f.channelCount(), this);
        playout_->open(QIODevice::ReadOnly);
        sink_->start(playout_);
    }
    // the microphone: 48 kHz mono Int16 if it gives that, else what it prefers, turned into that here
    const QAudioDevice in = QMediaDevices::defaultAudioInput();
    if (!in.isNull()) {
        QAudioFormat f = wanted(1);
        if (!in.isFormatSupported(f))
            f = in.preferredFormat();
        micChannels_ = qMax(1, f.channelCount());
        micRate_ = f.sampleRate() > 0 ? f.sampleRate() : kRate;
        micFloat_ = f.sampleFormat() == QAudioFormat::Float;
        if (f.sampleFormat() == QAudioFormat::Int16 || micFloat_) {
            source_ = new QAudioSource(in, f, this);
            mic_ = source_->start();
            if (mic_)
                connect(mic_, &QIODevice::readyRead, this, &CallAudio::readMicrophone);
        }
    }
}

void CallAudio::stop()
{
    if (source_) {
        source_->stop();
        delete source_;
        source_ = nullptr;
        mic_ = nullptr;
    }
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    delete playout_;
    playout_ = nullptr;
#ifdef PUDDLE_HAVE_OPUS
    if (encoder_)
        opus_encoder_destroy(static_cast<OpusEncoder *>(encoder_));
#endif
    encoder_ = nullptr;
    running_ = false;
    send_ = nullptr;
}

// What the microphone gave, as mono Int16 at 48 kHz, cut into 20 ms frames and encoded.
void CallAudio::readMicrophone()
{
    if (!mic_)
        return;
    const QByteArray raw = mic_->readAll();
    const int bytesPer = micFloat_ ? 4 : 2;
    const qsizetype frames = raw.size() / (bytesPer * micChannels_);
    const double step = double(micRate_) / kRate; // input samples per output sample
    for (; resampleAt_ < frames; resampleAt_ += step) {
        const qsizetype i = qsizetype(resampleAt_);
        double sum = 0;
        for (int c = 0; c < micChannels_; c++) {
            const char *p = raw.constData() + (i * micChannels_ + c) * bytesPer;
            if (micFloat_) {
                float v;
                memcpy(&v, p, 4);
                sum += v * 32767.0;
            } else {
                int16_t v;
                memcpy(&v, p, 2);
                sum += v;
            }
        }
        const int16_t s = int16_t(qBound(-32768.0, sum / micChannels_, 32767.0));
        captured_.append(reinterpret_cast<const char *>(&s), 2);
    }
    resampleAt_ -= double(frames);
    while (captured_.size() >= kFrameSamples * 2) {
        const QByteArray pcm = captured_.left(kFrameSamples * 2);
        captured_.remove(0, kFrameSamples * 2);
        if (muted_ || !send_)
            continue;
#ifdef PUDDLE_HAVE_OPUS
        unsigned char out[1500];
        int n = encoder_ ? opus_encode(static_cast<OpusEncoder *>(encoder_), reinterpret_cast<const int16_t *>(pcm.constData()), kFrameSamples, out, sizeof out)
                         : -1;
        if (n > 0)
            send_(QByteArray(reinterpret_cast<const char *>(out), n));
#endif
    }
}

QByteArray CallAudio::encodeForTest(const QByteArray &pcm)
{
#ifdef PUDDLE_HAVE_OPUS
    static OpusEncoder *enc = [] {
        int err = 0;
        OpusEncoder *e = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
        return e;
    }();
    unsigned char out[1500];
    int n = pcm.size() >= kFrameSamples * 2 ? opus_encode(enc, reinterpret_cast<const int16_t *>(pcm.constData()), kFrameSamples, out, sizeof out) : -1;
    return n > 0 ? QByteArray(reinterpret_cast<const char *>(out), n) : QByteArray();
#else
    Q_UNUSED(pcm);
    return {};
#endif
}

QByteArray CallAudio::decodeForTest(const QByteArray &frame)
{
#ifdef PUDDLE_HAVE_OPUS
    static OpusDecoder *dec = [] {
        int err = 0;
        return opus_decoder_create(kRate, 1, &err);
    }();
    int16_t pcm[kFrameSamples * 6];
    int n = opus_decode(dec, reinterpret_cast<const unsigned char *>(frame.constData()), frame.size(), pcm, kFrameSamples * 6, 0);
    return n > 0 ? QByteArray(reinterpret_cast<const char *>(pcm), n * 2) : QByteArray();
#else
    Q_UNUSED(frame);
    return {};
#endif
}
