// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "NvencEncoder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include <QElapsedTimer>
#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "krdp_logging.h"

namespace KRdp
{

// Key frames are only needed when the client recreates a surface, which asks
// for one, so they are rare and big frames don't cause regular stutters.
constexpr int GopSize = 600;
constexpr int ConversionThreads = 4;

static QString errorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

static SwsContext *createColorConversion(const QSize &size, AVPixelFormat format)
{
    SwsContext *sws = sws_alloc_context();
    if (!sws) {
        return nullptr;
    }
    av_opt_set_int(sws, "srcw", size.width(), 0);
    av_opt_set_int(sws, "srch", size.height(), 0);
    av_opt_set_int(sws, "src_format", format, 0);
    av_opt_set_int(sws, "dstw", size.width(), 0);
    av_opt_set_int(sws, "dsth", size.height(), 0);
    av_opt_set_int(sws, "dst_format", AV_PIX_FMT_NV12, 0);
    av_opt_set_int(sws, "sws_flags", SWS_POINT, 0);
    // RDP clients decode full range BT.601 (the default matrix), as KPipeWire
    // encodes for KRdp. NVENC itself would convert RGB to limited range.
    av_opt_set_int(sws, "src_range", 1, 0);
    av_opt_set_int(sws, "dst_range", 1, 0);
    // This is most of the work done on the CPU.
    av_opt_set_int(sws, "threads", std::clamp(int(std::thread::hardware_concurrency()), 1, ConversionThreads), 0);
    if (sws_init_context(sws, nullptr, nullptr) < 0) {
        sws_freeContext(sws);
        return nullptr;
    }
    return sws;
}

class KRDP_NO_EXPORT NvencEncoder::Private
{
public:
    void run(std::stop_token token, NvencEncoder *q);
    bool encodeImage(const QImage &image, bool keyFrame, NvencEncoder *q);
    bool open(const QSize &size, quint8 quality);
    void close();
    AVDictionary *codecOptions(quint8 quality) const;

    QByteArray codecName;
    std::atomic<quint8> quality = 75;

    std::mutex mutex;
    std::condition_variable_any condition;
    // Guarded by mutex.
    std::optional<QImage> pending;
    bool keyFrameRequested = false;

    // Only used on the encoding thread.
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *sws = nullptr;
    AVPixelFormat swsFormat = AV_PIX_FMT_NONE;
    QSize size;
    quint8 contextQuality = 0;
    QImage lastImage;
    QElapsedTimer clock;
    int64_t lastPts = -1;

    std::jthread thread;
};

NvencEncoder::NvencEncoder(const QByteArray &codecName)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->codecName = codecName;
    d->thread = std::jthread([this](std::stop_token token) {
        d->run(token, this);
    });
}

NvencEncoder::~NvencEncoder()
{
    d->thread.request_stop();
    d->thread.join();
    d->close();
}

bool NvencEncoder::isAvailable()
{
    static const bool available = [] {
        if (qEnvironmentVariableIntValue("KRDP_DISABLE_NVENC") != 0) {
            return false;
        }
        if (!avcodec_find_encoder_by_name("h264_nvenc")) {
            qCDebug(KRDP) << "FFmpeg was built without NVENC";
            return false;
        }
        // Without the driver NVENC can't work, don't try it on other GPUs.
        return QFile::exists(QStringLiteral("/proc/driver/nvidia/version"));
    }();
    return available;
}

void NvencEncoder::setQuality(quint8 quality)
{
    d->quality = quality;
}

void NvencEncoder::encode(const QImage &image)
{
    {
        std::lock_guard lock(d->mutex);
        d->pending = image;
    }
    d->condition.notify_one();
}

void NvencEncoder::requestKeyFrame()
{
    {
        std::lock_guard lock(d->mutex);
        d->keyFrameRequested = true;
    }
    d->condition.notify_one();
}

void NvencEncoder::Private::run(std::stop_token token, NvencEncoder *q)
{
    while (!token.stop_requested()) {
        QImage image;
        bool keyFrame = false;
        {
            std::unique_lock lock(mutex);
            if (!condition.wait(lock, token, [this] {
                    return pending.has_value() || keyFrameRequested;
                })) {
                return;
            }
            if (pending) {
                image = std::move(*pending);
                pending.reset();
            } else {
                image = lastImage;
            }
            keyFrame = std::exchange(keyFrameRequested, false);
        }

        // A key frame requested before the first frame, the first frame
        // will be one.
        if (image.isNull()) {
            continue;
        }

        lastImage = image;
        if (!encodeImage(image, keyFrame, q)) {
            close();
            Q_EMIT q->failed();
            return;
        }
    }
}

bool NvencEncoder::Private::encodeImage(const QImage &sourceImage, bool keyFrame, NvencEncoder *q)
{
    QImage image = sourceImage;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    switch (image.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        format = Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? AV_PIX_FMT_BGRA : AV_PIX_FMT_ARGB;
        break;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        format = AV_PIX_FMT_RGBA;
        break;
    default:
        image = image.convertToFormat(QImage::Format_RGB32);
        format = Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? AV_PIX_FMT_BGRA : AV_PIX_FMT_ARGB;
        break;
    }

    // H.264 needs even sizes, odd sizes are padded with black.
    const QSize evenSize((image.width() + 1) & ~1, (image.height() + 1) & ~1);
    if (evenSize != image.size()) {
        QImage padded(evenSize, image.format());
        padded.fill(Qt::black);
        for (int y = 0; y < image.height(); ++y) {
            memcpy(padded.scanLine(y), image.constScanLine(y), image.width() * 4);
        }
        image = padded;
    }

    const quint8 requestedQuality = quality;
    if (!context || sourceImage.size() != size || requestedQuality != contextQuality) {
        close();
        if (!open(sourceImage.size(), requestedQuality)) {
            return false;
        }
    }

    if (!sws || format != swsFormat) {
        sws_freeContext(sws);
        sws = createColorConversion(evenSize, format);
        if (!sws) {
            qCWarning(KRDP) << "Could not create the color conversion to NV12";
            return false;
        }
        swsFormat = format;
    }

    if (int result = av_frame_make_writable(frame); result < 0) {
        qCWarning(KRDP) << "Could not get a writable frame:" << errorString(result);
        return false;
    }

    AVFrame *source = av_frame_alloc();
    source->format = format;
    source->width = evenSize.width();
    source->height = evenSize.height();
    source->data[0] = const_cast<uint8_t *>(image.constBits());
    source->linesize[0] = image.bytesPerLine();
    const int converted = sws_scale_frame(sws, frame, source);
    av_frame_free(&source);
    if (converted < 0) {
        qCWarning(KRDP) << "Could not convert frame to NV12:" << errorString(converted);
        return false;
    }

    frame->pts = std::max<int64_t>(lastPts + 1, clock.elapsed());
    lastPts = frame->pts;
    frame->pict_type = keyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    if (int result = avcodec_send_frame(context, frame); result < 0) {
        qCWarning(KRDP) << "Could not encode frame:" << errorString(result);
        return false;
    }

    while (true) {
        const int result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            qCWarning(KRDP) << "Could not receive encoded frame:" << errorString(result);
            return false;
        }

        Q_EMIT q->packetReady(QByteArray(reinterpret_cast<const char *>(packet->data), packet->size), packet->flags & AV_PKT_FLAG_KEY, size);
        av_packet_unref(packet);
    }

    return true;
}

bool NvencEncoder::Private::open(const QSize &newSize, quint8 newQuality)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(codecName.constData());
    if (!codec) {
        qCWarning(KRDP) << "Encoder" << codecName << "not found";
        return false;
    }

    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!context || !frame || !packet) {
        qCWarning(KRDP) << "Could not allocate the encoder";
        return false;
    }

    context->width = (newSize.width() + 1) & ~1;
    context->height = (newSize.height() + 1) & ~1;
    context->pix_fmt = AV_PIX_FMT_NV12;
    context->time_base = AVRational{1, 1000};
    context->framerate = AVRational{60, 1};
    context->gop_size = GopSize;
    context->max_b_frames = 0;
    context->bit_rate = 0;
    context->color_range = AVCOL_RANGE_JPEG;
    context->colorspace = AVCOL_SPC_SMPTE170M;

    AVDictionary *options = codecOptions(newQuality);
    const int result = avcodec_open2(context, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
        qCWarning(KRDP) << "Could not open encoder" << codecName << "at size" << newSize << ":" << errorString(result);
        return false;
    }

    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    if (int result = av_frame_get_buffer(frame, 0); result < 0) {
        qCWarning(KRDP) << "Could not allocate frame:" << errorString(result);
        return false;
    }

    size = newSize;
    contextQuality = newQuality;
    clock.start();
    lastPts = -1;

    qCDebug(KRDP) << "Opened encoder" << codecName << "at size" << size << "with quality" << newQuality;
    return true;
}

AVDictionary *NvencEncoder::Private::codecOptions(quint8 quality) const
{
    // Same range as KPipeWire uses for libx264.
    const auto constantQuality = QByteArray::number(qRound(std::lerp(28.0, 17.0, quality / 100.0)));

    AVDictionary *options = nullptr;
    if (codecName == "h264_nvenc") {
        av_dict_set(&options, "preset", "p1", 0);
        av_dict_set(&options, "tune", "ull", 0);
        av_dict_set(&options, "profile", "baseline", 0);
        av_dict_set(&options, "rc", "vbr", 0);
        av_dict_set(&options, "cq", constantQuality.constData(), 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
    } else {
        // libx264, used by the tests.
        av_dict_set(&options, "preset", "ultrafast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
        av_dict_set(&options, "profile", "baseline", 0);
        av_dict_set(&options, "crf", constantQuality.constData(), 0);
        av_dict_set(&options, "forced-idr", "1", 0);
    }
    return options;
}

void NvencEncoder::Private::close()
{
    avcodec_free_context(&context);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws);
    sws = nullptr;
    swsFormat = AV_PIX_FMT_NONE;
    size = QSize();
}

}

#include "moc_NvencEncoder.cpp"
