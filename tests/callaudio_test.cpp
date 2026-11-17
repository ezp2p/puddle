// A call's sound without any devices (src/gui/app/callaudio.h): the jitter buffer's order, gaps, late frames, wrapping
// timestamps and catching up; Opus there and back, through the speaker's side, with a frame lost on the way; and the echo
// canceller taking a room's echo of the speaker out of the microphone, and leaving the voice at this end in.
#include "callaudio.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static constexpr uint32_t T = JitterBuffer::kFrameTicks;

static QByteArray tag(int n) { return QByteArray(1, char(n)); }

static int popTag(JitterBuffer &b, JitterBuffer::Kind *kind)
{
    QByteArray f;
    *kind = b.pop(&f);
    return *kind == JitterBuffer::Frame ? int((unsigned char)f[0]) : -1;
}

// 20 ms of a 440 Hz tone, mono Int16 at 48 kHz, starting at sample `from`
static QByteArray tone(int from)
{
    QByteArray pcm(960 * 2, '\0');
    auto *s = reinterpret_cast<int16_t *>(pcm.data());
    for (int i = 0; i < 960; i++)
        s[i] = int16_t(8000 * std::sin(2 * M_PI * 440 * (from + i) / 48000.0));
    return pcm;
}

static double loudness(const char *pcm, qint64 bytes)
{
    const auto *s = reinterpret_cast<const int16_t *>(pcm);
    double sum = 0;
    for (qint64 i = 0; i < bytes / 2; i++)
        sum += double(s[i]) * s[i];
    return bytes ? std::sqrt(sum / double(bytes / 2)) : 0;
}

// Something like a voice: harmonics of a pitch that wanders, in syllables of about 200 ms with pauses between
static std::vector<int16_t> voice(int samples, double pitch, double syllable, double level)
{
    std::vector<int16_t> v(static_cast<size_t>(samples));
    double phase = 0;
    for (int i = 0; i < samples; i++) {
        const double t = i / 48000.0;
        const double f0 = pitch * (1 + 0.15 * std::sin(2 * M_PI * 0.7 * t));
        phase += 2 * M_PI * f0 / 48000.0;
        double x = 0;
        for (int h = 1; h <= 12; h++)
            x += std::sin(h * phase) / h;
        const double env = std::max(0.0, std::sin(2 * M_PI * syllable * t)); // half the time talking
        v[size_t(i)] = int16_t(qBound(-32767.0, level * env * x, 32767.0));
    }
    return v;
}

static double energy(const std::vector<int16_t> &v, int from, int to)
{
    double e = 1;
    for (int i = from; i < to; i++)
        e += double(v[size_t(i)]) * v[size_t(i)];
    return e;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    JitterBuffer::Kind k;

    /* ---- waits for three frames, then plays them in order, whatever order they came in ---- */
    {
        JitterBuffer b(3);
        b.push(1000 + 1 * T, tag(2));
        b.push(1000, tag(1));
        CHECK(popTag(b, &k) == -1 && k == JitterBuffer::Wait); /* two is not enough yet */
        b.push(1000 + 2 * T, tag(3));
        CHECK(popTag(b, &k) == 1 && popTag(b, &k) == 2 && popTag(b, &k) == 3);
        CHECK(popTag(b, &k) == -1 && k == JitterBuffer::Wait); /* ran dry: waits to fill up again */
    }
    /* ---- a frame that never comes is lost (the decoder covers it); one that comes after its moment is dropped ---- */
    {
        JitterBuffer b(2);
        b.push(0, tag(1));
        b.push(2 * T, tag(3));
        CHECK(popTag(b, &k) == 1);
        CHECK(popTag(b, &k) == -1 && k == JitterBuffer::Lost);
        b.push(T, tag(2)); /* too late */
        CHECK(popTag(b, &k) == 3);
        CHECK(b.depth() == 0);
    }
    /* ---- timestamps wrap at 2^32 ---- */
    {
        JitterBuffer b(2);
        b.push(0xFFFFFFFFu - T + 1, tag(1));
        b.push(0xFFFFFFFFu - T + 1 + T, tag(2)); /* = 0 after wrapping */
        b.push(T, tag(3));
        CHECK(popTag(b, &k) == 1 && popTag(b, &k) == 2 && popTag(b, &k) == 3);
    }
    /* ---- far behind: keeps only the newest and carries on from there ---- */
    {
        JitterBuffer b(3);
        for (int i = 0; i < 40; i++)
            b.push(uint32_t(i) * T, tag(i));
        CHECK(b.depth() <= JitterBuffer::kMaxFrames);
        int first = popTag(b, &k);
        CHECK(first >= 40 - JitterBuffer::kMaxFrames);
    }
    /* ---- a long gap (the network was gone a while): no long run of guesses, it carries on from what came ---- */
    {
        JitterBuffer b(1);
        b.push(0, tag(1));
        CHECK(popTag(b, &k) == 1);
        b.push(50 * T, tag(2));
        int lost = 0;
        while (popTag(b, &k) == -1 && k == JitterBuffer::Lost)
            lost++;
        CHECK(k == JitterBuffer::Frame && lost == 0);
    }

    /* ---- the echo: the friend's voice from the speaker, back in the microphone 30 ms later through the room; then this end's
       own voice, alone and over the friend's. With a speaker that takes its sound in big gulps (PipeWire) and one that takes
       a little at a time and keeps 80 ms queued; the microphone's 20 ms come 5 ms late now and then ---- */
    if (EchoCanceller::built()) {
        struct Speaker { const char *name; int gulp, low; };
        for (Speaker sp : {Speaker{"gulps of 170 ms", 8192, 1024}, Speaker{"80 ms queued", 480, 3840}}) {
            const int C = EchoCanceller::kChunk, N = 14 * 48000;
            const std::vector<int16_t> far = voice(N + 48000, 140, 2.3, 6000);
            std::vector<int16_t> near = voice(N, 210, 1.7, 5000);
            for (int i = 0; i < 9 * 48000; i++) /* this end says nothing for the first 9 s, then talks */
                near[size_t(i)] = 0;
            std::vector<int16_t> mic(static_cast<size_t>(N)), out(mic);
            for (int i = 0; i < N; i++) {
                auto at = [&](int k) { return k >= 0 ? double(far[size_t(k)]) : 0.0; };
                const double room = 0.5 * at(i - 1440) + 0.25 * at(i - 1500) - 0.1 * at(i - 1700); /* 30 ms, two reflections */
                mic[size_t(i)] = int16_t(qBound(-32767.0, room + near[size_t(i)], 32767.0));
            }
            EchoCanceller ec;
            int handed = 0;
            uint32_t rnd = 1;
            for (int i = 0; i < N; i += 2 * C) { /* i: where the speaker is playing, and the microphone recording */
                while (handed - i < sp.low + 2 * C) { /* running low: takes another gulp */
                    ec.played(far.data() + handed, sp.gulp);
                    handed += sp.gulp;
                }
                rnd = rnd * 1103515245u + 12345u;
                if ((rnd >> 16) % 4 == 0) /* late: the speaker moved on by 5 ms before this was processed */
                    while (handed - i < sp.low + 2 * C + 240) {
                        ec.played(far.data() + handed, sp.gulp);
                        handed += sp.gulp;
                    }
                std::copy(mic.begin() + i, mic.begin() + i + 2 * C, out.begin() + i);
                ec.process(out.data() + i, 2 * C);
            }
            /* only the friend talking (from 4 s, once it has settled, to 9 s): most of the echo gone */
            const double erle = 10 * std::log10(energy(mic, 4 * 48000, 9 * 48000) / energy(out, 4 * 48000, 9 * 48000));
            /* this end talking over the friend (from 10 s): its own voice still there. By loudness, as the canceller's filters
               shift the waveform a little: what is left of the echo is far below this (see above) */
            const double nearKept = std::sqrt(energy(out, 10 * 48000, N) / energy(near, 10 * 48000, N)); /* 1 = all of it */
            printf("callaudio_test: echo canceller %s, speaker %s: echo down %.1f dB, near voice kept %.2f, moved %d\n",
                   EchoCanceller::kind(), sp.name, erle, nearKept, ec.resyncs());
            CHECK(erle > 15);
            CHECK(nearKept > 0.5 && nearKept < 1.5);
            CHECK(ec.resyncs() <= 1); /* settles within the first gulp, and stays */
        }
        /* the reference never passes what the speaker was handed; and catches up when it stays well behind */
        EchoCanceller drift;
        int16_t chunk[EchoCanceller::kChunk] = {}, sound[EchoCanceller::kChunk] = {};
        for (int i = 0; i < 10; i++) {
            drift.played(sound, EchoCanceller::kChunk);
            drift.process(chunk, EchoCanceller::kChunk);
        }
        CHECK(drift.resyncs() == 0);
        for (int i = 0; i < 20; i++) /* the speaker took 200 ms more and keeps it */
            drift.played(sound, EchoCanceller::kChunk);
        for (int i = 0; i < 700; i++) {
            drift.played(sound, EchoCanceller::kChunk);
            drift.process(chunk, EchoCanceller::kChunk);
        }
        CHECK(drift.resyncs() == 1);
    } else {
        printf("callaudio_test: built without an echo canceller: not tested\n");
    }

    if (!CallAudio::built()) {
        printf("callaudio_test: ok (built without Opus: the sound itself not tested)\n");
        return failures ? 1 : 0;
    }

    /* ---- Opus there and back ---- */
    {
        QByteArray frame = CallAudio::encodeForTest(tone(0));
        CHECK(frame.size() > 0 && frame.size() < 400);
        QByteArray pcm = CallAudio::decodeForTest(frame);
        CHECK(pcm.size() == 960 * 2);
    }
    /* ---- through the speaker's side: 25 frames of a tone, one lost on the way, played as 500 ms without a hole ---- */
    {
        Mixer m;
        for (int i = 0; i < 25; i++)
            if (i != 12)
                m.push(QStringLiteral("bob"), uint32_t(i) * T, CallAudio::encodeForTest(tone(i * 960)));
        Playout out(&m, 2);
        out.open(QIODevice::ReadOnly);
        QByteArray all(25 * 960 * 2 * 2, '\0');
        CHECK(out.read(all.data(), all.size()) == all.size());
        /* the start is the codec settling; after it, every 20 ms is the tone (the lost one covered), left and right the same */
        bool steady = true;
        for (int i = 3; i < 25; i++)
            steady = steady && loudness(all.constData() + qsizetype(i) * 960 * 4, 960 * 4) > 2000;
        CHECK(steady);
        const auto *s = reinterpret_cast<const int16_t *>(all.constData());
        bool stereo = true;
        for (int i = 0; i < 25 * 960; i++)
            stereo = stereo && s[2 * i] == s[2 * i + 1];
        CHECK(stereo);
        /* and after the last frame, silence rather than nothing: a speaker that reads nothing stops */
        QByteArray after(960 * 4, 'x');
        CHECK(out.read(after.data(), after.size()) == after.size() && loudness(after.constData(), after.size()) < 50);
    }

    /* ---- two voices (a group call): each its own buffer and decoder, played together; one gone quiet is let go ---- */
    {
        Mixer m;
        for (int i = 0; i < 10; i++) {
            m.push(QStringLiteral("bob"), 5000 + uint32_t(i) * T, CallAudio::encodeForTest(tone(i * 960)));
            m.push(QStringLiteral("carol"), 90000 + uint32_t(i) * T, CallAudio::encodeForTest(tone(i * 960))); /* her own clock */
        }
        CHECK(m.voices() == 2);
        Mixer one;
        for (int i = 0; i < 10; i++)
            one.push(QStringLiteral("bob"), uint32_t(i) * T, CallAudio::encodeForTest(tone(i * 960)));
        int16_t both[960], alone[960];
        double lb = 0, la = 0;
        for (int i = 0; i < 8; i++) {
            m.pull(both);
            one.pull(alone);
            if (i >= 3) { /* once the codec has settled */
                lb += loudness(reinterpret_cast<const char *>(both), sizeof both);
                la += loudness(reinterpret_cast<const char *>(alone), sizeof alone);
            }
        }
        CHECK(lb > 1.8 * la && la > 2000 * 5); /* the same tone twice, in step: about twice as loud */
        for (int i = 0; i < 520; i++) /* 10 s of nothing more from either */
            m.pull(both);
        CHECK(m.voices() == 0);
    }

    /* ---- the talk gate (a voice channel): voice activation opens on speech and holds a little after; push to talk only while
     * held; a call's always open ---- */
    {
        int16_t speech[960], hush[960];
        for (int i = 0; i < 960; i++) {
            speech[i] = int16_t(8000 * std::sin(i * 2 * M_PI * 220 / 48000)); /* about -15 dBFS */
            hush[i] = int16_t((i % 7) - 3);                                   /* a room's hum, far below the threshold */
        }
        CHECK(TalkGate::level(speech, 960) > -20 && TalkGate::level(speech, 960) < -10);
        CHECK(TalkGate::level(hush, 960) < -70);
        TalkGate g;
        CHECK(g.pass(hush, 960, 0) && g.pass(speech, 960, 20)); /* Open: whatever it hears */
        g.setMode(TalkGate::VoiceActivity);
        CHECK(!g.pass(hush, 960, 1000) && !g.talking());
        CHECK(g.pass(speech, 960, 1020) && g.talking());
        CHECK(g.pass(hush, 960, 1040) && g.pass(hush, 960, 1300)); /* the end of a word: held kHangMs */
        CHECK(!g.pass(hush, 960, 1340) && !g.talking());
        g.setThreshold(-5); /* less sensitive: the same speech isn't loud enough */
        CHECK(!g.pass(speech, 960, 5000));
        g.setMode(TalkGate::PushToTalk);
        CHECK(!g.pass(speech, 960, 6000));
        g.setPushed(true);
        CHECK(g.pass(hush, 960, 6020) && !g.talking()); /* held: it goes, though there is nothing to hear */
        g.setThreshold(TalkGate::kDefaultThreshold);
        CHECK(g.pass(speech, 960, 6040) && g.talking());
        g.setPushed(false);
        CHECK(!g.pass(speech, 960, 6060) && !g.talking());
    }

    /* ---- who is talking: those whose frames came in the last moment ---- */
    {
        Mixer m;
        const QByteArray f = CallAudio::encodeForTest(QByteArray(960 * 2, 0));
        m.push("ann", 960, f);
        m.push("ben", 960, f);
        CHECK(m.speaking() == (QStringList{ "ann", "ben" }));
        usleep(350 * 1000);
        m.push("ben", 1920, f);
        CHECK(m.speaking() == QStringList{ "ben" });
        CHECK(m.speaking(1000).size() == 2);
    }

    if (failures) {
        fprintf(stderr, "callaudio_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("callaudio_test: ok\n");
    return 0;
}
