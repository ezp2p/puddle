#include "qrimage.h"

#include <QPainter>
#include <QUrl>

#ifdef PUDDLE_HAVE_QRENCODE
#include <qrencode.h>
#endif

bool QrImageProvider::available()
{
#ifdef PUDDLE_HAVE_QRENCODE
    return true;
#else
    return false;
#endif
}

QImage QrImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const int want = requestedSize.width() > 0 ? requestedSize.width() : 256;
    QImage img(want, want, QImage::Format_RGB32);
    img.fill(Qt::white);
#ifdef PUDDLE_HAVE_QRENCODE
    const QByteArray text = QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
    QRcode *qr = QRcode_encodeString(text.constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (qr) {
        const int quiet = 4, n = qr->width + 2 * quiet;
        const double cell = double(want) / n;
        QPainter p(&img);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        for (int y = 0; y < qr->width; y++)
            for (int x = 0; x < qr->width; x++)
                if (qr->data[y * qr->width + x] & 1)
                    p.drawRect(QRectF((x + quiet) * cell, (y + quiet) * cell, cell + 0.5, cell + 0.5));
        QRcode_free(qr);
    }
#else
    (void)id;
#endif
    if (size)
        *size = img.size();
    return img;
}
