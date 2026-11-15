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

    // Each Opus frame from the friend, with its RTP timestamp. Called on a network thread: it must not block.
    void setOnFrame(std::function<void(uint32_t ts, const QByteArray &frame)> fn);
    // One Opus frame of 20 ms from the microphone. Any thread. false if the leg can't take it (not open yet).
    bool send(const QByteArray &frame);

signals:
    void connectedChanged();

public:
    struct Impl;

private:
    std::shared_ptr<Impl> d_;
};
