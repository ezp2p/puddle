#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

// A phone's own WebRTC leg to its Puddle, for the call the phone has (docs/calls.md, "On a phone"). The phone offers, over
// the signed device API (POST /api/v1/calls/{call}/media), and the Puddle answers; the Puddle passes Opus frames between
// this leg and the friend. Nothing trickles: the offer and the answer each come complete with their candidates, and the
// phone adds the Puddle's own address (the one it reaches the Puddle at) at the UDP port the answer names. The DTLS
// fingerprint each side checks travels in a request the phone signed and an answer the Puddle signed, so the media is
// bound to the pairing.
//
// In a group call the members' frames come over a data channel the offer also opens, each marked with the member's number
// (`slot` in the call's members): [slot][RTP timestamp, big-endian][frame]. The phone's own still go on the track.
//
// Video (docs/calls.md, "Video"): a track "video" both ways (the phone's camera; the friend's back), and seven that only come
// in, "v1" .. "v7", a group call's members by their number. Each sending track says its SSRC, so the Puddle can tell them
// apart.
//
// Built without libdatachannel, built() is false and a leg never opens: calls still ring and connect on the phone, silent.
class CallLeg : public QObject {
    Q_OBJECT
public:
    explicit CallLeg(QObject *parent = nullptr);
    ~CallLeg() override;

    static bool built();

    // A new leg for call `id` (closing any other); `ready` gets the offer on this object's thread once it is complete, or
    // "" if one can't be made.
    void open(const QString &id, std::function<void(const QString &sdp)> ready);
    // The Puddle's answer. `host` is the Puddle's address as this phone reaches it, `udp` the port its calls use.
    bool answer(const QString &sdp, const QString &host, int udp);
    void close();

    QString callId() const;
    bool connected() const;

    // Each Opus frame from the friend ("" as `from`), or a member of a group call ("slot:<n>"), with its RTP timestamp in
    // that one's count. Called on a network thread: it must not block.
    void setOnFrame(std::function<void(const QString &from, uint32_t ts, const QByteArray &frame)> fn);
    // Each video frame (an H.264 access unit), from "" or "slot:<n>" as above. A network thread: it must not block.
    void setOnVideo(std::function<void(const QString &from, uint32_t ts, const QByteArray &au)> fn);
    // An access unit from the camera. Any thread. false if the leg can't take it yet.
    bool sendVideo(const QByteArray &au);
    // One Opus frame of 20 ms from the microphone. Any thread. false if the leg can't take it (not open yet).
    bool send(const QByteArray &frame);

signals:
    void connectedChanged();

public:
    struct Impl;

private:
    std::shared_ptr<Impl> d_;
};
