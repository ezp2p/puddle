// A call's sound without any devices (src/gui/app/callaudio.h): the jitter buffer's order, gaps, late frames, wrapping
// timestamps and catching up; and Opus there and back, through the speaker's side, with a frame lost on the way.
#include "callaudio.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <cstring>

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
        JitterBuffer b(3);
        for (int i = 0; i < 25; i++)
            if (i != 12)
                b.push(uint32_t(i) * T, CallAudio::encodeForTest(tone(i * 960)));
        Playout out(&b, 2);
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

    if (failures) {
        fprintf(stderr, "callaudio_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("callaudio_test: ok\n");
    return 0;
}
