// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Av1Encoder.h"

#include <algorithm>

#include <freerdp/codec/av1.h>
#include <freerdp/codec/h264.h>

#include "PixelFormat_p.h"
#include "krdp_logging.h"

namespace KRdp
{

static bool hasHardwareEncoder(FREERDP_AV1_CONTEXT *context)
{
    return context && freerdp_av1_context_get_option(context, FREERDP_AV1_CONTEXT_OPTION_HARDWARE) == 1;
}

class KRDP_NO_EXPORT Av1Encoder::Private
{
public:
    bool encodeImage(const QImage &image, bool keyFrame, quint8 quality, Av1Encoder *q);
    bool open(const QSize &size, quint8 quality);
    void close();

    // Only used on the encoding thread.
    FREERDP_AV1_CONTEXT *context = nullptr;
    QSize size;
    quint8 contextQuality = 0;
};

Av1Encoder::Av1Encoder()
    : d(std::make_unique<Private>())
{
    start();
}

Av1Encoder::~Av1Encoder()
{
    stop();
    close();
}

bool Av1Encoder::isAvailable()
{
    static const bool available = [] {
        if (qEnvironmentVariableIntValue("KRDP_DISABLE_AV1") != 0) {
            return false;
        }
        FREERDP_AV1_CONTEXT *context = freerdp_av1_context_new(TRUE);
        const bool hardware = hasHardwareEncoder(context);
        freerdp_av1_context_free(context);
        if (!hardware) {
            qCDebug(KRDP) << "No GPU encodes AV1, AV1 is not offered to clients";
        }
        return hardware;
    }();
    return available;
}

void Av1Encoder::close()
{
    d->close();
}

bool Av1Encoder::encodeImage(const QImage &image, bool keyFrame)
{
    return d->encodeImage(image, keyFrame, quality(), this);
}

bool Av1Encoder::Private::encodeImage(const QImage &sourceImage, bool keyFrame, quint8 requestedQuality, Av1Encoder *q)
{
    QImage image = sourceImage;
    auto pixelFormat = toPixelFormat(image.format());
    if (!pixelFormat) {
        image = image.convertToFormat(QImage::Format_RGB32);
        pixelFormat = toPixelFormat(image.format());
    }

    if (!context || image.size() != size || requestedQuality != contextQuality) {
        close();
        if (!open(image.size(), requestedQuality)) {
            return false;
        }
        // The first frame of an encoder is a key frame.
        keyFrame = true;
    } else if (keyFrame && !freerdp_av1_context_set_option(context, FREERDP_AV1_CONTEXT_OPTION_KEYFRAME, 1)) {
        qCWarning(KRDP) << "Could not request an AV1 key frame";
        return false;
    }

    const RECTANGLE_16 rect = {0, 0, UINT16(image.width()), UINT16(image.height())};
    BYTE *data = nullptr;
    UINT32 length = 0;
    // The region of the frame, KRdp sends its own.
    RDPGFX_H264_METABLOCK meta = {};
    const INT32 result =
        freerdp_av1_compress(context, image.constBits(), *pixelFormat, image.bytesPerLine(), image.width(), image.height(), &rect, &data, &length, &meta);
    free_h264_metablock(&meta);
    if (result < 0) {
        qCWarning(KRDP) << "Could not encode AV1 frame of size" << image.size();
        return false;
    }
    if (result == 0 || !data || length == 0) {
        // Nothing to send yet.
        return true;
    }

    q->emitPacket(QByteArray(reinterpret_cast<const char *>(data), length), keyFrame, size);
    return true;
}

bool Av1Encoder::Private::open(const QSize &newSize, quint8 newQuality)
{
    context = freerdp_av1_context_new(TRUE);
    if (!context) {
        qCWarning(KRDP) << "Could not create the AV1 encoder";
        return false;
    }

    // Bound quality-mode bursts on variable-capacity links. The default is
    // 8 Mbit/s per megapixel, capped at 60 Mbit/s. Zero explicitly opts out.
    bool bitrateSet = false;
    const int configuredBitrate = qEnvironmentVariableIntValue("KRDP_AV1_BITRATE", &bitrateSet);
    const quint32 bitrate = bitrateSet && configuredBitrate >= 0
        ? std::min(configuredBitrate, 200000)
        : std::clamp(qint64(newSize.width()) * newSize.height() * 8 / 1000, qint64(4000), qint64(60000));
    // AV1 main profile, constant quality subject to the bitrate ceiling.
    if (!freerdp_av1_context_set_option(context, FREERDP_AV1_CONTEXT_OPTION_PROFILE, 0)
        || !freerdp_av1_context_set_option(context, FREERDP_AV1_CONTEXT_OPTION_RATECONTROL, FREERDP_AV1_CQ)
        || !freerdp_av1_context_set_option(context, FREERDP_AV1_CONTEXT_OPTION_BITRATE, bitrate)
        || !freerdp_av1_context_set_option(context, FREERDP_AV1_CONTEXT_OPTION_QUALITY, newQuality)) {
        qCWarning(KRDP) << "Could not configure the AV1 encoder";
        return false;
    }
    // FreeRDP encodes in software when the GPU can't, too slow for a desktop.
    if (!freerdp_av1_context_reset(context, newSize.width(), newSize.height()) || !hasHardwareEncoder(context)) {
        qCWarning(KRDP) << "The GPU can not encode AV1 at size" << newSize;
        return false;
    }

    size = newSize;
    contextQuality = newQuality;
    qCInfo(KRDP) << "Opened AV1 encoder at size" << size << "with quality" << newQuality << "max kbit/s" << bitrate;
    return true;
}

void Av1Encoder::Private::close()
{
    freerdp_av1_context_free(context);
    context = nullptr;
    size = QSize();
}

}

#include "moc_Av1Encoder.cpp"
