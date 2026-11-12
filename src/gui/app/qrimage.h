#pragma once

#include <QQuickImageProvider>

// "image://qr/<percent-encoded text>": the text as a QR code, black on white with a quiet zone, for the pairing link
// (docs/identity.md, "Devices"). Built only where libqrencode is found; `available()` says whether it was.
class QrImageProvider : public QQuickImageProvider {
public:
    QrImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    static bool available();
};
