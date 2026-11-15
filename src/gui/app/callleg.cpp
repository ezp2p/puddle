#include "callleg.h"

#include <QMetaObject>

#ifdef PUDDLE_HAVE_CALLS

#include <rtc/rtc.hpp>

#include <QRandomGenerator>

#include <mutex>

#define OPUS_PT 111
#define MID "audio"

// Everything the connection's own threads touch, behind one lock. `gen` tells a callback of a leg that was closed (or
// replaced) from one of the leg there is now.
struct CallLeg::Impl {
    std::mutex mu;
    CallLeg *owner = nullptr; // cleared when the CallLeg goes, under the lock
    quint64 gen = 0;
    QString id;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dc;
    bool open = false, connected = false, offered = false;
    uint32_t nextTs = 0;
    std::function<void(const QString &, uint32_t, const QByteArray &)> onFrame;

    // Run `fn` on the owner's thread, if it is still there.
    void post(std::function<void(CallLeg *)> fn)
    {
        std::lock_guard<std::mutex> g(mu);
        if (CallLeg *o = owner)
            QMetaObject::invokeMethod(o, [o, fn = std::move(fn)] { fn(o); }, Qt::QueuedConnection);
    }
};

bool CallLeg::built() { return true; }

CallLeg::CallLeg(QObject *parent) : QObject(parent), d_(std::make_shared<Impl>())
{
    d_->owner = this;
}

CallLeg::~CallLeg()
{
    close();
    std::lock_guard<std::mutex> g(d_->mu);
    d_->owner = nullptr;
    d_->onFrame = nullptr;
}

QString CallLeg::callId() const
{
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->id;
}

bool CallLeg::connected() const
{
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->connected;
}

void CallLeg::setOnFrame(std::function<void(const QString &, uint32_t, const QByteArray &)> fn)
{
    std::lock_guard<std::mutex> g(d_->mu);
    d_->onFrame = std::move(fn);
}

void CallLeg::close()
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dc;
    bool was;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        d_->gen++;
        pc.swap(d_->pc);
        track.swap(d_->track);
        dc.swap(d_->dc);
        was = d_->connected;
        d_->id.clear();
        d_->open = d_->connected = d_->offered = false;
    }
    if (track)
        track->resetCallbacks();
    if (dc)
        dc->resetCallbacks();
    if (pc) {
        pc->resetCallbacks();
        pc->close();
    }
    if (was)
        emit connectedChanged();
}

void CallLeg::open(const QString &id, std::function<void(const QString &)> ready)
{
    close();
    std::weak_ptr<Impl> weak = d_;
    quint64 gen;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        gen = d_->gen;
        d_->id = id;
        d_->nextTs = QRandomGenerator::system()->generate();
    }
    try {
        rtc::Configuration conf; // no ICE servers: the phone and its Puddle find each other by their own addresses
        conf.disableAutoNegotiation = true; // one offer, made below once the track and the data channel are both in it
        auto pc = std::make_shared<rtc::PeerConnection>(conf);
        rtc::Description::Audio media(MID, rtc::Description::Direction::SendRecv);
        media.addOpusCodec(OPUS_PT);
        auto track = pc->addTrack(media);
        auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(QRandomGenerator::system()->generate(), "puddle", OPUS_PT,
                                                                 rtc::OpusRtpPacketizer::DefaultClockRate);
        auto pack = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
        pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        pack->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        track->setMediaHandler(pack);

        track->onOpen([weak, gen] {
            if (auto d = weak.lock()) {
                std::lock_guard<std::mutex> g(d->mu);
                if (d->gen == gen)
                    d->open = true;
            }
        });
        track->onFrame([weak, gen](rtc::binary data, rtc::FrameInfo info) {
            auto d = weak.lock();
            if (!d || data.empty())
                return;
            std::function<void(const QString &, uint32_t, const QByteArray &)> fn;
            {
                std::lock_guard<std::mutex> g(d->mu);
                if (d->gen != gen)
                    return;
                fn = d->onFrame;
            }
            if (fn)
                fn(QString(), info.timestamp, QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())));
        });
        // a group call's members, each frame marked with whose: as fresh as can be, so nothing is sent again or waited for
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        auto dc = pc->createDataChannel("frames", init);
        dc->onMessage([weak, gen](rtc::message_variant m) {
            auto *bin = std::get_if<rtc::binary>(&m);
            auto d = weak.lock();
            if (!d || !bin || bin->size() < 6)
                return;
            std::function<void(const QString &, uint32_t, const QByteArray &)> fn;
            {
                std::lock_guard<std::mutex> g(d->mu);
                if (d->gen != gen)
                    return;
                fn = d->onFrame;
            }
            if (!fn)
                return;
            const auto *b = reinterpret_cast<const unsigned char *>(bin->data());
            const uint32_t ts = uint32_t(b[1]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 8 | b[4];
            fn(QStringLiteral("slot:%1").arg(b[0]), ts, QByteArray(reinterpret_cast<const char *>(b + 5), qsizetype(bin->size() - 5)));
        });
        pc->onStateChange([weak, gen](rtc::PeerConnection::State s) {
            auto d = weak.lock();
            if (!d)
                return;
            bool changed = false;
            {
                std::lock_guard<std::mutex> g(d->mu);
                if (d->gen != gen)
                    return;
                const bool now = s == rtc::PeerConnection::State::Connected;
                changed = now != d->connected;
                d->connected = now;
            }
            if (changed)
                d->post([](CallLeg *o) { emit o->connectedChanged(); });
        });
        // the offer goes once it is complete: candidates and all
        pc->onGatheringStateChange([weak, gen, ready](rtc::PeerConnection::GatheringState s) {
            auto d = weak.lock();
            if (!d || s != rtc::PeerConnection::GatheringState::Complete)
                return;
            QString sdp;
            {
                std::lock_guard<std::mutex> g(d->mu);
                if (d->gen != gen || d->offered || !d->pc)
                    return;
                auto desc = d->pc->localDescription();
                if (!desc)
                    return;
                d->offered = true;
                sdp = QString::fromStdString(std::string(*desc));
            }
            d->post([gen, ready, sdp](CallLeg *o) {
                if (o->d_->gen == gen) // read on the owner's thread, which alone changes it
                    ready(sdp);
            });
        });
        {
            std::lock_guard<std::mutex> g(d_->mu);
            d_->pc = pc;
            d_->track = track;
            d_->dc = dc;
        }
        pc->setLocalDescription();
    } catch (const std::exception &) {
        close();
        ready(QString());
    }
}

bool CallLeg::answer(const QString &sdp, const QString &host, int udp)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        pc = d_->pc;
    }
    if (!pc)
        return false;
    try {
        pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "answer"));
        // the Puddle's address as this phone reaches it (its candidates name only its own interfaces): resolved by
        // libdatachannel if it is a name
        if (!host.isEmpty() && udp > 0 && udp < 65536) {
            QString h = host;
            if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']')))
                h = h.mid(1, h.size() - 2);
            const std::string cand = "a=candidate:9 1 UDP 2122000000 " + h.toStdString() + " " + std::to_string(udp) + " typ host";
            try {
                pc->addRemoteCandidate(rtc::Candidate(cand, MID));
            } catch (const std::exception &) {
            }
        }
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

bool CallLeg::send(const QByteArray &frame)
{
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        if (!d_->open || !d_->track || frame.isEmpty())
            return false;
        track = d_->track;
        ts = d_->nextTs;
        d_->nextTs += 960; // 20 ms at 48 kHz
    }
    try {
        track->sendFrame(reinterpret_cast<const rtc::byte *>(frame.constData()), size_t(frame.size()), rtc::FrameInfo(ts));
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

#else // built without libdatachannel: no leg, so a phone's calls are silent

struct CallLeg::Impl {};

bool CallLeg::built() { return false; }
CallLeg::CallLeg(QObject *parent) : QObject(parent) {}
CallLeg::~CallLeg() = default;
QString CallLeg::callId() const { return QString(); }
bool CallLeg::connected() const { return false; }
void CallLeg::setOnFrame(std::function<void(const QString &, uint32_t, const QByteArray &)>) {}
void CallLeg::close() {}
void CallLeg::open(const QString &, std::function<void(const QString &)> ready) { ready(QString()); }
bool CallLeg::answer(const QString &, const QString &, int) { return false; }
bool CallLeg::send(const QByteArray &) { return false; }

#endif
