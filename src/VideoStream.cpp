// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-graphics-pipeline.c from Gnome Remote
// Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoStream.h"
#include "VideoStream_p.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>

#include <fcntl.h>
#include <unistd.h>

#include <QDateTime>
#include <QHash>
#include <QQueue>
#include <QScopeGuard>

#include <DmaBufHandler>
#include <PipeWireEncodedStream>
#include <freerdp/codec/progressive.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/update.h>
#include <kpipewire_version.h>
#include <qassert.h>

#include "Av1Encoder.h"
#include "GfxCapabilities.h"
#include "NetworkDetection.h"
#include "NvencEncoder.h"
#include "PeerContext_p.h"
#include "PixelFormat_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

constexpr auto FrameRateEstimateAveragePeriod = clk::seconds(1);
// The smallest change of the requested frame rate that is applied.
constexpr int MinimumFrameRateChange = 2;
#if KPIPEWIRE_VERSION >= QT_VERSION_CHECK(6, 7, 90)
// The number of frames of a monitor waiting to be sent above which KPipeWire
// drops frames before encoding them, and below which it encodes them again.
constexpr qsizetype HighQueueCount = 3;
constexpr qsizetype LowQueueCount = 1;
#endif
constexpr uint32_t ProgressiveCodecContextId = 1;
BOOL gfxChannelIdAssigned(RdpgfxServerContext *context, uint32_t channelId)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    if (stream->onChannelIdAssigned(channelId)) {
        return TRUE;
    }
    return FALSE;
}

uint32_t gfxCapsAdvertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onCapsAdvertise(capsAdvertise);
}

uint32_t gfxFrameAcknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto stream = reinterpret_cast<VideoStream *>(context->custom);
    return stream->onFrameAcknowledge(frameAcknowledge);
}

uint32_t gfxQoEFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU *)
{
    return CHANNEL_RC_OK;
}

struct Surface {
    uint16_t id = 0;
    uint32_t codecContextId = 0;
    QSize size;
    VideoFrameGate gate;
};

struct FrameRateEstimate {
    clk::steady_clock::time_point timeStamp;
    int estimate = 0;
};

/**
 * Memory for the images frames are copied into, reused once their images are
 * gone. Allocating images as big as a frame anew costs page faults and having
 * the memory cleared every frame.
 */
class ImagePool : public std::enable_shared_from_this<ImagePool>
{
public:
    /**
     * An image of \p size in \p format with four bytes per pixel, its
     * contents undefined.
     */
    QImage take(const QSize &size, QImage::Format format)
    {
        if (size.isEmpty()) {
            return QImage();
        }

        const qsizetype bytesPerLine = qsizetype(size.width()) * 4;
        const qsizetype bytes = bytesPerLine * size.height();

        auto buffer = std::make_unique<Buffer>();
        buffer->pool = shared_from_this();
        {
            std::lock_guard lock(mutex);
            if (bytes != bufferSize) {
                freeMemory.clear();
                bufferSize = bytes;
            }
            if (!freeMemory.empty()) {
                buffer->memory = std::move(freeMemory.back());
                freeMemory.pop_back();
            }
        }
        if (!buffer->memory) {
            buffer->memory = std::make_unique_for_overwrite<uchar[]>(bytes);
        }
        buffer->size = bytes;

        uchar *data = buffer->memory.get();
        return QImage(data, size.width(), size.height(), bytesPerLine, format, &ImagePool::release, buffer.release());
    }

private:
    // Usually one image is taken for every one that is released.
    static constexpr std::size_t MaximumFreeBuffers = 2;

    struct Buffer {
        std::shared_ptr<ImagePool> pool;
        std::unique_ptr<uchar[]> memory;
        qsizetype size = 0;
    };

    // Called when the last copy of an image is gone, on any thread.
    static void release(void *info)
    {
        std::unique_ptr<Buffer> buffer(static_cast<Buffer *>(info));
        auto &pool = *buffer->pool;
        std::lock_guard lock(pool.mutex);
        if (buffer->size == pool.bufferSize && pool.freeMemory.size() < MaximumFreeBuffers) {
            pool.freeMemory.push_back(std::move(buffer->memory));
        }
    }

    std::mutex mutex;
    qsizetype bufferSize = 0;
    std::vector<std::unique_ptr<uchar[]>> freeMemory;
};

class KRDP_NO_EXPORT VideoStream::Private
{
public:
    using RdpGfxContextPtr = std::unique_ptr<RdpgfxServerContext, decltype(&rdpgfx_server_context_free)>;
    using ProgressiveContextPtr = std::unique_ptr<PROGRESSIVE_CONTEXT, decltype(&progressive_context_free)>;

    /**
     * The PipeWire stream of one monitor. Only used on the main thread.
     */
    struct Stream {
        ~Stream()
        {
            if (encodedStream) {
                encodedStream->stop();
            }
            if (sourceStream) {
                sourceStream->setActive(false);
            }
            if (pipeWireFd > 0) {
                ::close(pipeWireFd);
            }
        }

        // PipeWire streams take ownership of the fd they are given, so every
        // stream we create gets its own duplicate of pipeWireFd.
        int duplicatePipeWireFd() const
        {
            if (pipeWireFd <= 0) {
                return pipeWireFd;
            }
            return fcntl(pipeWireFd, F_DUPFD_CLOEXEC, 3);
        }

        std::unique_ptr<PipeWireEncodedStream> encodedStream;
        std::unique_ptr<PipeWireSourceStream> sourceStream;
        // Encodes the frames of sourceStream: AV1, or H.264 on NVIDIA GPUs.
        std::unique_ptr<FrameEncoder> encoder;
        quint32 nodeId = 0;
        int pipeWireFd = -1;
        QSize size;
        // Set when the source changed size and the encoder needs to be
        // restarted, packets from the old encoder are dropped until then.
        bool encoderRestartPending = false;
        // The latest raw frame, to repaint the surface after it was recreated.
        QImage lastImage;
        const std::shared_ptr<ImagePool> imagePool = std::make_shared<ImagePool>();
    };

    /**
     * The surface of one monitor. Shared with the frame submission thread,
     * guarded by outputsMutex.
     */
    struct Output {
        Surface surface;
        // Position of the monitor in the client's framebuffer.
        QPoint origin;
        // The size the client expects, used until the stream size is known.
        QSize expectedSize;
        // Size of the latest frame.
        QSize size;
    };

    /**
     * The desktop needed to fit all outputs, starting at the origin.
     * Must be called with outputsMutex held.
     */
    QRect desktopRect() const
    {
        QRect rect;
        for (const auto &output : outputs) {
            const QSize size = output.size.isValid() ? output.size : output.expectedSize;
            if (size.isValid()) {
                rect |= QRect(output.origin, size);
            }
        }
        return QRect(QPoint(0, 0), QSize(rect.right() + 1, rect.bottom() + 1));
    }

    RdpConnection *session;
    std::optional<EncodingMode> activeEncodingMode;
    std::vector<std::unique_ptr<Stream>> streams;
    // The monitor the cursor was last seen on.
    int cursorMonitor = 0;
    DmaBufHandler dmaBufHandler;
    // NVENC did not work, KPipeWire encodes instead.
    bool nvencFailed = false;
    // AV1 did not work, fallbackEncodingMode is used instead. Read on the
    // connection thread.
    std::atomic_bool av1Failed = false;
    EncodingMode fallbackEncodingMode = EncodingMode::Progressive;

    RdpGfxContextPtr gfxContext = RdpGfxContextPtr(nullptr, rdpgfx_server_context_free);
    ProgressiveContextPtr progressive = ProgressiveContextPtr(nullptr, progressive_context_free);

    uint32_t frameId = 0;
    uint32_t channelId = 0;

    uint16_t nextSurfaceId = 1;

    std::mutex outputsMutex;
    std::vector<Output> outputs;
    // The desktop we last told the client about.
    QRect desktop;
    bool pendingReset = true;

    // Set from the connection thread, read from the encoding and frame
    // submission threads.
    std::atomic_bool enabled = false;
    bool streamingEnabled = false;
    std::atomic_bool capsConfirmed = false;
    bool channelOpen = false;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;
    // Notified when a frame is queued or the frame submission thread may be
    // able to send one again: a frame was acknowledged or the capabilities
    // were confirmed.
    std::condition_variable_any frameQueueCondition;

    QQueue<VideoFrame> frameQueue;
    QHash<uint32_t, clk::steady_clock::time_point> pendingFrames;

    std::mutex pendingFramesMutex;

    int maximumFrameRate = 60;
    std::atomic_int inFlightLimit = 2;
    std::atomic<qint64> acknowledgementUs = 1000;
    std::atomic_int requestedFrameRate = 60;
    QQueue<FrameRateEstimate> frameRateEstimates;
    clk::steady_clock::time_point lastFrameRateEstimation;

    std::atomic_uint64_t sentFrames = 0;
    std::atomic_uint64_t sentBytes = 0;
    std::atomic_uint64_t acknowledgedFrames = 0;
    std::atomic_uint64_t sentKeyFrames = 0;
    clk::steady_clock::time_point lastStatistics = clk::steady_clock::now();
    bool initialized = false;
    quint8 quality = 100;
};

static QString encodingModeName(VideoStream::EncodingMode mode)
{
    switch (mode) {
    case VideoStream::EncodingMode::H264:
        return QStringLiteral("h264");
    case VideoStream::EncodingMode::AV1:
        return QStringLiteral("av1");
    case VideoStream::EncodingMode::Progressive:
        return QStringLiteral("progressive");
    }
    Q_UNREACHABLE();
}

/**
 * Whether \p mode sends a video stream, which needs key frames.
 */
static bool isVideo(std::optional<VideoStream::EncodingMode> mode)
{
    return mode == VideoStream::EncodingMode::H264 || mode == VideoStream::EncodingMode::AV1;
}

bool VideoStream::h264Disabled()
{
    static const bool h264Disabled = qEnvironmentVariableIntValue("KRDP_DISABLE_H264") != 0;
    return h264Disabled;
}

static RECTANGLE_16 toRectangle16(const QRect &rect)
{
    RECTANGLE_16 result = {};
    result.left = rect.left();
    result.top = rect.top();
    result.right = rect.right() + 1;
    result.bottom = rect.bottom() + 1;
    return result;
}

/**
 * A copy of the frame, PipeWire reuses its memory for later frames.
 */
static QImage copyImage(const PipeWireFrameData &data, ImagePool &pool)
{
    // What compositors usually provide is laid out like Format_RGB32 in
    // memory, copying it as is avoids the channel swap and conversion of
    // toImage().
    if (Q_BYTE_ORDER == Q_LITTLE_ENDIAN && (data.format == SPA_VIDEO_FORMAT_BGRx || data.format == SPA_VIDEO_FORMAT_BGRA)
        && data.stride >= data.size.width() * 4) {
        QImage image = pool.take(data.size, QImage::Format_RGB32);
        uchar *bits = image.bits();
        const auto source = static_cast<const uchar *>(data.data);
        const qsizetype rowSize = qsizetype(data.size.width()) * 4;
        for (int y = 0; y < data.size.height(); ++y) {
            memcpy(bits + y * image.bytesPerLine(), source + qsizetype(y) * data.stride, rowSize);
        }
        return image;
    }
    return data.toImage().convertToFormat(QImage::Format_RGB32);
}

static std::optional<REGION16> toRegion16(const QRegion &region, const QRect &frameRect)
{
    REGION16 invalidRegion = {};
    region16_init(&invalidRegion);

    const QRegion clipped = region.isEmpty() ? QRegion(frameRect) : region.intersected(frameRect);
    for (const QRect &rect : clipped) {
        if (!rect.isValid()) {
            continue;
        }

        const RECTANGLE_16 rectangle = toRectangle16(rect);
        if (!region16_union_rect(&invalidRegion, &invalidRegion, &rectangle)) {
            region16_uninit(&invalidRegion);
            return std::nullopt;
        }
    }

    if (region16_is_empty(&invalidRegion)) {
        const RECTANGLE_16 fullFrame = toRectangle16(frameRect);
        if (!region16_union_rect(&invalidRegion, &invalidRegion, &fullFrame)) {
            region16_uninit(&invalidRegion);
            return std::nullopt;
        }
    }

    return invalidRegion;
}

VideoStream::VideoStream(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
    // A single monitor until told otherwise.
    d->streams.push_back(std::make_unique<Private::Stream>());
    d->outputs.resize(1);
}

void VideoStream::setActiveEncodingMode(EncodingMode mode)
{
    if (d->activeEncodingMode == mode) {
        return;
    }

    {
        // Read by queueFrame() on the encoding threads.
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
        d->activeEncodingMode = mode;
    }

    for (std::size_t monitor = 0; monitor < d->streams.size(); ++monitor) {
        createStream(monitor);
    }

    // NVENC and AV1 encoders queue their frames themselves, until they are
    // destroyed above.
    std::lock_guard lock(d->frameQueueMutex);
    d->frameQueue.clear();
}

void VideoStream::createStream(int monitor)
{
    auto &stream = *d->streams.at(monitor);

    if (stream.encodedStream) {
        stream.encodedStream->stop();
        stream.encodedStream.reset();
    }
    if (stream.sourceStream) {
        stream.sourceStream->setActive(false);
        stream.sourceStream.reset();
    }
    stream.encoder.reset();
    stream.encoderRestartPending = false;

    if (!d->activeEncodingMode) {
        return;
    }

    const bool useAv1 = *d->activeEncodingMode == EncodingMode::AV1;
    const bool useNvenc = *d->activeEncodingMode == EncodingMode::H264 && NvencEncoder::isAvailable() && !d->nvencFailed;

    if (*d->activeEncodingMode == EncodingMode::H264 && !useNvenc) {
        stream.encodedStream = std::make_unique<PipeWireEncodedStream>();
        stream.encodedStream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        stream.encodedStream->setColorRange(PipeWireBaseEncodedStream::ColorRange::Full);
        stream.encodedStream->setEncoder(PipeWireEncodedStream::H264Baseline);
        stream.encodedStream->setQuality(d->quality);
        stream.encodedStream->setMaxFramerate(d->requestedFrameRate, 1);
        stream.encodedStream->setMaxPendingFrames(d->requestedFrameRate);

        connect(stream.encodedStream.get(), &PipeWireEncodedStream::newPacket, this, [this, monitor](const PipeWireEncodedStream::Packet &packet) {
            onPacketReceived(monitor, packet);
        });
        connect(stream.encodedStream.get(), &PipeWireEncodedStream::sizeChanged, this, [this, monitor](const QSize &size) {
            // The encoder is set up once for the initial size of the stream,
            // so when the source changes size (e.g. a resized virtual monitor)
            // it needs to be recreated.
            auto &stream = *d->streams.at(monitor);
            const bool restartNeeded = stream.size.isValid() && stream.size != size;
            setStreamSize(monitor, size);
            if (restartNeeded && !stream.encoderRestartPending) {
                stream.encoderRestartPending = true;
                QMetaObject::invokeMethod(
                    this,
                    [this, monitor]() {
                        restartEncoder(monitor);
                    },
                    Qt::QueuedConnection);
            }
        });
        connect(stream.encodedStream.get(), &PipeWireEncodedStream::cursorChanged, this, [this, monitor](const PipeWireCursor &cursor) {
            onCursorChanged(monitor, cursor);
        });
        if (stream.nodeId != 0) {
            stream.encodedStream->setNodeId(stream.nodeId);
            if (stream.pipeWireFd > 0) {
                stream.encodedStream->setFd(stream.duplicatePipeWireFd());
            }
        }
        if (d->streamingEnabled && stream.nodeId != 0) {
            stream.encodedStream->start();
        }
    } else {
        if (useAv1) {
            qCInfo(KRDP) << "Encoding monitor" << monitor << "with AV1";
            stream.encoder = std::make_unique<Av1Encoder>();
        } else if (useNvenc) {
            qCInfo(KRDP) << "Encoding monitor" << monitor << "with NVENC";
            stream.encoder = std::make_unique<NvencEncoder>();
        }
        if (stream.encoder) {
            stream.encoder->setQuality(d->quality);
            // Queued for sending right on the encoding thread, rather than
            // waiting for the main thread, which may be busy downloading the
            // next frame. The encoder stops its thread when it is destroyed,
            // so no packets of an encoder that was replaced arrive afterwards.
            connect(
                stream.encoder.get(),
                &FrameEncoder::packetReady,
                stream.encoder.get(),
                [this, monitor, useAv1](const QByteArray &data, bool isKeyFrame, const QSize &size, const std::shared_ptr<void> &sent) {
                    VideoFrame frame;
                    frame.monitor = monitor;
                    frame.size = size;
                    frame.data = data;
                    frame.isAv1 = useAv1;
                    frame.isKeyFrame = isKeyFrame;
                    frame.sent = sent;
                    queueFrame(frame);
                },
                Qt::DirectConnection);
            connect(stream.encoder.get(), &FrameEncoder::failed, stream.encoder.get(), [this, useAv1]() {
                if (useAv1) {
                    qCWarning(KRDP) << "AV1 does not work, falling back to" << encodingModeName(d->fallbackEncodingMode);
                    d->av1Failed = true;
                } else {
                    qCWarning(KRDP) << "NVENC does not work, falling back to KPipeWire for H.264 encoding";
                    d->nvencFailed = true;
                }
                // Not from within a signal of the encoder that gets destroyed.
                QMetaObject::invokeMethod(
                    this,
                    [this, useAv1]() {
                        if (useAv1) {
                            // The surfaces of the other codec are created anew.
                            setActiveEncodingMode(d->fallbackEncodingMode);
                            reset();
                            return;
                        }
                        for (std::size_t monitor = 0; monitor < d->streams.size(); ++monitor) {
                            createStream(monitor);
                        }
                    },
                    Qt::QueuedConnection);
            });
        }

        stream.sourceStream = std::make_unique<PipeWireSourceStream>();
        // Frames end up in memory either way, see onFrameReceived(). With
        // DMA-BUF, KPipeWire offers every format with all modifiers of the GPU,
        // on NVIDIA that does not fit its 4 KiB buffer for the formats. What
        // does not fit is dropped, in an order that changes with every start,
        // and when that is BGRx and BGRA, the only formats KWin offers,
        // negotiating the stream fails with "no more input formats".
        stream.sourceStream->setAllowDmaBuf(false);
        stream.sourceStream->setDamageEnabled(true);
        stream.sourceStream->setMaxFramerate({static_cast<quint32>(d->requestedFrameRate.load()), 1});
        // Handled right away, while PipeWire does not use the buffer of the
        // frame: once it is given back the compositor may draw the next frame
        // into it, and it saves a pass through the event loop.
        connect(stream.sourceStream.get(), &PipeWireSourceStream::frameReceived, this, [this, monitor](const PipeWireFrame &frame) {
            onFrameReceived(monitor, frame);
            if (frame.cursor) {
                onCursorChanged(monitor, *frame.cursor);
            }
        });
        connect(stream.sourceStream.get(), &PipeWireSourceStream::streamParametersChanged, this, [this, monitor]() {
            setStreamSize(monitor, d->streams.at(monitor)->sourceStream->size());
        });

        if (stream.nodeId != 0 && stream.pipeWireFd) {
            if (!stream.sourceStream->createStream(stream.nodeId, stream.duplicatePipeWireFd())) {
                qCWarning(KRDP) << "Could not create PipeWire source stream" << stream.sourceStream->error();
                d->session->close(RdpConnection::CloseReason::VideoInitFailed);
                return;
            }
            stream.size = stream.sourceStream->size();
        }
        stream.sourceStream->setActive(d->streamingEnabled && stream.nodeId != 0);
    }
}

void VideoStream::setStreamSize(int monitor, const QSize &size)
{
    auto &stream = *d->streams.at(monitor);
    if (stream.size == size) {
        return;
    }

    stream.size = size;
    Q_EMIT monitorSizeChanged(monitor, size);
    if (monitor == 0) {
        Q_EMIT sizeChanged(size);
    }
}

void VideoStream::onCursorChanged(int monitor, const PipeWireCursor &cursor)
{
    // Every monitor reports the cursor, those that don't show it report an
    // empty one. Only follow the monitor the cursor is on.
    if (!cursor.texture.isNull()) {
        d->cursorMonitor = monitor;
    } else if (monitor != d->cursorMonitor) {
        return;
    }
    Q_EMIT cursorChanged(cursor);
}

VideoStream::~VideoStream()
{
    close();
}

void VideoStream::restartEncoder(int monitor)
{
    if (monitor < 0 || monitor >= int(d->streams.size()) || !isVideo(d->activeEncodingMode)) {
        return;
    }

    if (const auto &encoder = d->streams.at(monitor)->encoder) {
        encoder->requestKeyFrame();
        return;
    }

    qCDebug(KRDP) << "Restarting encoder of monitor" << monitor << "at size" << d->streams.at(monitor)->size;
    createStream(monitor);
}

void VideoStream::refreshMonitor(int monitor)
{
    if (monitor < 0 || monitor >= int(d->streams.size())) {
        return;
    }

    qCDebug(KRDP) << "Repainting monitor" << monitor << "after its surface was recreated";

    if (isVideo(d->activeEncodingMode)) {
        // A new encoder starts with a key frame.
        restartEncoder(monitor);
        return;
    }

    const auto &image = d->streams.at(monitor)->lastImage;
    if (image.isNull()) {
        return;
    }
    VideoFrame frame;
    frame.monitor = monitor;
    frame.size = image.size();
    frame.image = image;
    frame.damage = QRegion(QRect(QPoint(0, 0), image.size()));
    queueFrame(frame);
}

void VideoStream::setMonitorLayout(const MonitorLayout &layout)
{
    const std::size_t count = std::max<std::size_t>(1, layout.size());

    while (d->streams.size() > count) {
        d->streams.pop_back();
    }
    while (d->streams.size() < count) {
        d->streams.push_back(std::make_unique<Private::Stream>());
        createStream(d->streams.size() - 1);
    }

    {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.removeIf([count](const VideoFrame &frame) {
            return frame.monitor >= int(count);
        });
    }

    std::lock_guard lock(d->outputsMutex);
    bool resetNeeded = d->outputs.size() != count;
    while (d->outputs.size() > count) {
        destroySurface(d->outputs.back().surface);
        d->outputs.pop_back();
    }
    d->outputs.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const QPoint origin = i < std::size_t(layout.size()) ? layout[i].rect.topLeft() : QPoint();
        resetNeeded |= d->outputs[i].origin != origin;
        d->outputs[i].origin = origin;
        d->outputs[i].expectedSize = i < std::size_t(layout.size()) ? layout[i].rect.size() : QSize();
    }
    // Size changes reset the graphics only when a frame at the new size
    // arrives, after the virtual output resize was debounced and applied.
    d->pendingReset |= resetNeeded;
}

int VideoStream::monitorCount() const
{
    return d->streams.size();
}

bool VideoStream::initialize()
{
    if (d->initialized) {
        return true;
    }

    d->gfxContext.reset(rdpgfx_server_context_new(contextForPeer(d->session->rdpPeer())->virtualChannelManager));
    if (!d->gfxContext) {
        qCWarning(KRDP) << "Failed to create graphics pipeline context";
        return false;
    }

    d->gfxContext->custom = this;
    d->gfxContext->ChannelIdAssigned = gfxChannelIdAssigned;
    d->gfxContext->CapsAdvertise = gfxCapsAdvertise;
    d->gfxContext->FrameAcknowledge = gfxFrameAcknowledge;
    d->gfxContext->QoeFrameAcknowledge = gfxQoEFrameAcknowledge;
    d->gfxContext->rdpcontext = d->session->rdpPeerContext();

    if (!d->gfxContext->Initialize(d->gfxContext.get(), FALSE)) {
        qCWarning(KRDP) << "Failed to initialize graphics pipeline context";
        d->gfxContext.reset();
        return false;
    }

    d->progressive.reset(progressive_context_new(TRUE));
    if (!d->progressive) {
        qCWarning(KRDP) << "Failed to create progressive codec context";
        d->gfxContext.reset();
        return false;
    }

    d->initialized = true;

    connect(d->session->networkDetection(), &NetworkDetection::rttChanged, this, &VideoStream::updateRequestedFrameRate);

    d->frameSubmissionThread = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            VideoFrame nextFrame;
            {
                std::unique_lock lock(d->frameQueueMutex);
                if (!d->frameQueueCondition.wait(lock, token, [this] {
                        return !d->frameQueue.isEmpty() && d->capsConfirmed && hasInFlightCapacity();
                    })) {
                    break;
                }
                nextFrame = d->frameQueue.takeFirst();
            }
            sendFrame(nextFrame);
#if KPIPEWIRE_VERSION >= QT_VERSION_CHECK(6, 7, 90)
            // Frames encoded by KPipeWire, its encoder may wait for them to be sent.
            if (!nextFrame.data.isEmpty() && !nextFrame.sent) {
                QMetaObject::invokeMethod(
                    this,
                    [this, monitor = nextFrame.monitor]() {
                        updateBackpressure(monitor);
                    },
                    Qt::QueuedConnection);
            }
#endif
        }
    });

    qCDebug(KRDP) << "Video stream initialized with H.264" << (h264Disabled() ? "disabled" : "enabled");

    return true;
}

void VideoStream::close()
{
    // PipeWire may only be used on the main thread, but the connection closes
    // the stream from its own thread. Not waiting for the main thread there,
    // as it may be waiting for the connection thread to finish.
    QMetaObject::invokeMethod(
        this,
        [this]() {
            for (const auto &stream : d->streams) {
                if (stream->encodedStream) {
                    stream->encodedStream->stop();
                }
                if (stream->sourceStream) {
                    stream->sourceStream->setActive(false);
                }
                // Consumer GPUs only have a few NVENC sessions.
                stream->encoder.reset();
            }
        },
        Qt::AutoConnection);

    if (d->frameSubmissionThread.joinable()) {
        d->frameSubmissionThread.request_stop();
        d->frameSubmissionThread.join();
    }

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
    }
    {
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
        // Read by queueFrame() on the encoding threads.
        d->activeEncodingMode.reset();
    }

    destroySurfaces();

    if (d->gfxContext) {
        if (d->channelOpen) {
            d->gfxContext->Close(d->gfxContext.get());
            d->channelOpen = false;
        }
        d->gfxContext.reset();
    }
    d->initialized = false;

    Q_EMIT closed();
}

void VideoStream::queueFrame(const KRdp::VideoFrame &frame)
{
    if (d->session->state() != RdpConnection::State::Streaming || !d->enabled) {
        return;
    }

    // Woken up after the lock is released, at the end of the function.
    auto notify = qScopeGuard([this]() {
        d->frameQueueCondition.notify_one();
    });

    std::lock_guard lock(d->frameQueueMutex);
    if (isVideo(d->activeEncodingMode)) {
        // Frames before a key frame of the same monitor are not needed anymore.
        if (frame.isKeyFrame) {
            d->frameQueue.removeIf([&frame](const VideoFrame &queued) {
                return queued.monitor == frame.monitor;
            });
        }
        d->frameQueue.append(frame);
        return;
    } else if (d->activeEncodingMode == EncodingMode::Progressive) {
        // for the raster path we only need to keep the latest frame of each
        // monitor, but accumulate damage
        QRegion lastDamage;
        auto it = std::find_if(d->frameQueue.rbegin(), d->frameQueue.rend(), [&frame](const VideoFrame &queued) {
            return queued.monitor == frame.monitor;
        });
        if (it != d->frameQueue.rend()) {
            lastDamage = it->damage;
            d->frameQueue.erase(std::next(it).base());
        }
        KRdp::VideoFrame nextFrame = frame;
        nextFrame.damage += lastDamage;
        d->frameQueue.append(std::move(nextFrame));
    }
}

void VideoStream::reset()
{
    std::lock_guard lock(d->outputsMutex);
    d->pendingReset = true;
}

bool VideoStream::enabled() const
{
    return d->enabled;
}

void VideoStream::setEnabled(bool enabled)
{
    if (d->enabled == enabled) {
        return;
    }

    d->enabled = enabled;
    Q_EMIT enabledChanged();

    // H.264 frames were dropped while disabled. Called from the connection
    // thread, the streams belong to the main thread.
    if (enabled) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                for (const auto &stream : d->streams) {
                    if (stream->encoder) {
                        stream->encoder->requestKeyFrame();
                    }
                }
            },
            Qt::QueuedConnection);
    }
}

void VideoStream::requestKeyFrame(int monitor)
{
    // Called from the connection and frame submission threads, the streams
    // belong to the main thread.
    QMetaObject::invokeMethod(
        this,
        [this, monitor]() {
            if (monitor < int(d->streams.size()) && d->streams.at(monitor)->encoder) {
                d->streams.at(monitor)->encoder->requestKeyFrame();
            }
        },
        Qt::QueuedConnection);
}

void VideoStream::setStreamingEnabled(bool enabled)
{
    if (d->streamingEnabled == enabled) {
        return;
    }

    d->streamingEnabled = enabled;
    for (const auto &stream : d->streams) {
        if (stream->encodedStream) {
            if (enabled && stream->nodeId != 0) {
                stream->encodedStream->start();
            } else {
                stream->encodedStream->stop();
            }
        }
        if (stream->sourceStream) {
            stream->sourceStream->setActive(enabled && stream->nodeId != 0);
        }
    }
}

void VideoStream::setVideoQuality(quint8 quality)
{
    d->quality = quality;
    for (const auto &stream : d->streams) {
        if (stream->encodedStream) {
            stream->encodedStream->setQuality(quality);
        }
        if (stream->encoder) {
            stream->encoder->setQuality(quality);
        }
    }
}

void VideoStream::setPipeWireSource(quint32 nodeId, int fd)
{
    setPipeWireSource(0, nodeId, fd);
}

void VideoStream::setPipeWireSource(int monitor, quint32 nodeId, int fd)
{
    if (monitor < 0 || monitor >= int(d->streams.size())) {
        qCWarning(KRDP) << "PipeWire source for unknown monitor" << monitor;
        if (fd > 0) {
            ::close(fd);
        }
        return;
    }

    auto &stream = *d->streams.at(monitor);
    stream.nodeId = nodeId;
    if (stream.pipeWireFd > 0 && stream.pipeWireFd != fd) {
        ::close(stream.pipeWireFd);
    }
    stream.pipeWireFd = fd;
    if (stream.encodedStream) {
        stream.encodedStream->setNodeId(nodeId);
        if (stream.pipeWireFd > 0) {
            stream.encodedStream->setFd(stream.duplicatePipeWireFd());
        }
        if (d->streamingEnabled) {
            stream.encodedStream->start();
        }
    }
    if (stream.sourceStream) {
        if (!stream.sourceStream->createStream(nodeId, stream.duplicatePipeWireFd())) {
            qCWarning(KRDP) << "Could not create PipeWire source stream" << stream.sourceStream->error();
            d->session->close(RdpConnection::CloseReason::VideoInitFailed);
            return;
        }
        setStreamSize(monitor, stream.sourceStream->size());
        stream.sourceStream->setActive(d->streamingEnabled);
    }
}

bool VideoStream::onChannelIdAssigned(uint32_t channelId)
{
    d->channelId = channelId;

    return true;
}

uint32_t VideoStream::onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise)
{
    // Windows clients (mstsc) send CapsAdvertise twice: once during
    // initial setup and again after confirming. If we already confirmed
    // caps, this is a GFX channel reset — clear surface state so
    // surfaces get re-created on the next frame.
    if (d->capsConfirmed) {
        qCDebug(KRDP) << "GFX channel reset (re-advertisement), resetting surface state";
        d->capsConfirmed = false;
        destroySurfaces();
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.clear();
    }

    const bool av1 = !d->av1Failed && Av1Encoder::isAvailable();
    const auto capabilities = chooseGfxCapabilities(capsAdvertise->capsSets, capsAdvertise->capsSetCount, !h264Disabled(), av1);
    if (!capabilities) {
        qCWarning(KRDP) << "Client advertised no usable graphics capability sets";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    EncodingMode negotiatedMode = EncodingMode::Progressive;
    switch (capabilities->codec) {
    case GfxCodec::H264:
        negotiatedMode = EncodingMode::H264;
        break;
    case GfxCodec::Av1:
        negotiatedMode = EncodingMode::AV1;
        break;
    case GfxCodec::Progressive:
        break;
    }
    // Only set here, it is read on the main thread once the encoder failed.
    d->fallbackEncodingMode = capabilities->h264Supported ? EncodingMode::H264 : EncodingMode::Progressive;

    QMetaObject::invokeMethod(
        this,
        [this, negotiatedMode]() {
            setActiveEncodingMode(negotiatedMode);
        },
        Qt::BlockingQueuedConnection); // RDP callbacks are on the connection thread, VideoStream operates on the main thread
    qCDebug(KRDP) << "Selected encoding mode:" << encodingModeName(negotiatedMode);
    qCDebug(KRDP) << "Selected caps:" << capVersionToString(capabilities->capsSet.version);

    RDPGFX_CAPSET capsSet = capabilities->capsSet;
    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu = {};
    capsConfirmPdu.capsSet = &capsSet;
    const UINT status = d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "CapsConfirm failed" << status;
        return status;
    }

    d->capsConfirmed = true;
    wakeFrameSubmission();

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto id = frameAcknowledge->frameId;

    {
        std::lock_guard lock(d->pendingFramesMutex);

        auto itr = d->pendingFrames.constFind(id);
        if (itr == d->pendingFrames.cend()) {
            qCWarning(KRDP) << "Got frame acknowledge for an unknown frame";
            return CHANNEL_RC_OK;
        }

        const auto elapsed = std::max<qint64>(1000, clk::duration_cast<clk::microseconds>(clk::steady_clock::now() - itr.value()).count());
        const auto sample = std::min<qint64>(elapsed, 10000000);
        d->acknowledgementUs = (7 * d->acknowledgementUs.load() + sample) / 8;
        ++d->acknowledgedFrames;
        d->pendingFrames.erase(itr);
    }

    // Send the next frame right away.
    wakeFrameSubmission();

    return CHANNEL_RC_OK;
}

void VideoStream::wakeFrameSubmission()
{
    {
        // Not missing the notification while the frame submission thread
        // checks whether it can send a frame.
        std::lock_guard lock(d->frameQueueMutex);
    }
    d->frameQueueCondition.notify_one();
}

void VideoStream::unregisterFrame(uint32_t frameId)
{
    std::lock_guard lock(d->pendingFramesMutex);
    d->pendingFrames.remove(frameId);
}

void VideoStream::onPacketReceived(int monitor, const PipeWireEncodedStream::Packet &data)
{
    const auto &stream = *d->streams.at(monitor);
    if (stream.encoderRestartPending) {
        // Encoded at the old size, the client would fail to decode it.
        return;
    }

    VideoFrame frameData;
    frameData.monitor = monitor;
    frameData.size = stream.size;
    frameData.data = data.data();
    frameData.isKeyFrame = data.isKeyFrame();
    queueFrame(frameData);
    updateBackpressure(monitor);
}

void VideoStream::updateBackpressure(int monitor)
{
#if KPIPEWIRE_VERSION >= QT_VERSION_CHECK(6, 7, 90)
    // Encoded H.264 frames can't be dropped anymore. When the client takes
    // them slower than KPipeWire encodes them they queue up, adding latency
    // until the next key frame. Instead KPipeWire drops frames before encoding
    // them while many are queued, and encodes the latest one once few are left.
    if (monitor < 0 || monitor >= int(d->streams.size())) {
        return;
    }
    const auto &encodedStream = d->streams.at(monitor)->encodedStream;
    if (!encodedStream) {
        return;
    }

    qsizetype queued = 0;
    {
        std::lock_guard lock(d->frameQueueMutex);
        queued = std::count_if(d->frameQueue.cbegin(), d->frameQueue.cend(), [monitor](const VideoFrame &frame) {
            return frame.monitor == monitor;
        });
    }

    const bool paused = encodedStream->encoderPaused();
    if (!paused && queued > HighQueueCount) {
        qCDebug(KRDP) << "Pausing the encoder of monitor" << monitor << "with" << queued << "frames queued";
        encodedStream->setEncoderPaused(true);
    } else if (paused && queued < LowQueueCount) {
        qCDebug(KRDP) << "Resuming the encoder of monitor" << monitor;
        encodedStream->setEncoderPaused(false);
    }
#else
    Q_UNUSED(monitor);
#endif
}

void VideoStream::onFrameReceived(int monitor, const PipeWireFrame &data)
{
    // Frames that only move the cursor have no image.
    if (!data.dataFrame && !data.dmabuf) {
        return;
    }

    if (monitor >= int(d->streams.size())) {
        return;
    }
    auto &stream = *d->streams.at(monitor);

    VideoFrame frameData;
    frameData.monitor = monitor;

    frameData.size = data.dataFrame ? data.dataFrame->size : QSize(data.dmabuf ? data.dmabuf->width : 0, data.dmabuf ? data.dmabuf->height : 0);
    frameData.damage = data.damage.value_or(QRegion(QRect(QPoint(0, 0), frameData.size)));
    if (data.presentationTimestamp) {
        frameData.presentationTimeStamp = clk::system_clock::time_point(clk::duration_cast<clk::microseconds>(*data.presentationTimestamp));
    }

    if (data.dataFrame) {
        frameData.image = copyImage(*data.dataFrame, *stream.imagePool);
    } else {
        QImage image = stream.imagePool->take(frameData.size, QImage::Format_RGBA8888_Premultiplied);
        if (!d->dmaBufHandler.downloadFrame(image, data)) {
            qCWarning(KRDP) << "Failed to download DMA-BUF frame";
            return;
        }
        frameData.image = std::move(image);
    }

    if (stream.encoder) {
        stream.encoder->encode(frameData.image);
        return;
    }
    stream.lastImage = frameData.image;
    queueFrame(frameData);
}

bool VideoStream::openChannel()
{
    if (!d->gfxContext) {
        return false;
    }
    if (d->channelOpen) {
        return true;
    }

    if (!d->gfxContext->Open(d->gfxContext.get())) {
        qCWarning(KRDP) << "Failed to open RDPGFX dynamic channel";
        return false;
    }

    d->channelOpen = true;
    return true;
}

void VideoStream::destroySurface(Surface &surface)
{
    if (surface.id == 0) {
        return;
    }

    if (d->gfxContext && surface.codecContextId != 0) {
        RDPGFX_DELETE_ENCODING_CONTEXT_PDU deleteEncodingContextPdu = {};
        deleteEncodingContextPdu.surfaceId = surface.id;
        deleteEncodingContextPdu.codecContextId = surface.codecContextId;
        const UINT status = d->gfxContext->DeleteEncodingContext(d->gfxContext.get(), &deleteEncodingContextPdu);
        if (status != CHANNEL_RC_OK && status != CHANNEL_RC_NOT_INITIALIZED) {
            qCWarning(KRDP) << "DeleteEncodingContext failed" << status;
        }
    }

    if (d->gfxContext) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurfacePdu = {};
        deleteSurfacePdu.surfaceId = surface.id;
        const UINT status = d->gfxContext->DeleteSurface(d->gfxContext.get(), &deleteSurfacePdu);
        if (status != CHANNEL_RC_OK && status != CHANNEL_RC_NOT_INITIALIZED) {
            qCWarning(KRDP) << "DeleteSurface failed" << status;
        }
    }

    if (d->progressive) {
        progressive_delete_surface_context(d->progressive.get(), surface.id);
    }

    surface = Surface{};
}

void VideoStream::destroySurfaces()
{
    std::lock_guard lock(d->outputsMutex);
    for (auto &output : d->outputs) {
        destroySurface(output.surface);
    }
    d->desktop = QRect();
    d->pendingReset = true;
}

bool VideoStream::createSurface(int monitor)
{
    auto &output = d->outputs.at(monitor);
    destroySurface(output.surface);

    const QSize size = output.size;

    RDPGFX_CREATE_SURFACE_PDU createSurfacePdu = {};
    createSurfacePdu.width = size.width();
    createSurfacePdu.height = size.height();
    uint16_t surfaceId = d->nextSurfaceId++;
    createSurfacePdu.surfaceId = surfaceId;
    createSurfacePdu.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
    UINT status = d->gfxContext->CreateSurface(d->gfxContext.get(), &createSurfacePdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "CreateSurface failed" << status << "surface" << surfaceId << "size" << size;
        return false;
    }

    output.surface = Surface{
        .id = surfaceId,
        .codecContextId = d->activeEncodingMode == EncodingMode::Progressive ? ProgressiveCodecContextId : 0,
        .size = size,
    };

    if (d->activeEncodingMode == EncodingMode::Progressive) {
        if (progressive_create_surface_context(d->progressive.get(), surfaceId, size.width(), size.height()) < 0) {
            qCWarning(KRDP) << "Failed to create progressive surface context";
            destroySurface(output.surface);
            return false;
        }
    }

    // Place the surface where the monitor is in the client's framebuffer.
    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurfaceToOutputPdu = {};
    mapSurfaceToOutputPdu.outputOriginX = output.origin.x();
    mapSurfaceToOutputPdu.outputOriginY = output.origin.y();
    mapSurfaceToOutputPdu.surfaceId = surfaceId;
    status = d->gfxContext->MapSurfaceToOutput(d->gfxContext.get(), &mapSurfaceToOutputPdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "MapSurfaceToOutput failed" << status << "surface" << surfaceId;
        destroySurface(output.surface);
        return false;
    }

    return true;
}

void VideoStream::performReset(const QRect &desktop, int monitor)
{
    if (!d->gfxContext) {
        auto settings = d->session->rdpPeerContext()->settings;
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desktop.width());
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, desktop.height());
        d->session->rdpPeerContext()->update->DesktopResize(d->session->rdpPeerContext());
        d->outputs.at(monitor).surface.size = d->outputs.at(monitor).size;
        d->desktop = desktop;
        return;
    }

    for (auto &output : d->outputs) {
        destroySurface(output.surface);
    }

    std::vector<MONITOR_DEF> monitors;
    for (std::size_t i = 0; i < d->outputs.size(); ++i) {
        const auto &output = d->outputs[i];
        const QSize size = output.size.isValid() ? output.size : output.expectedSize;
        if (!size.isValid()) {
            continue;
        }
        MONITOR_DEF monitorDef = {};
        monitorDef.left = output.origin.x();
        monitorDef.top = output.origin.y();
        monitorDef.right = output.origin.x() + size.width() - 1;
        monitorDef.bottom = output.origin.y() + size.height() - 1;
        monitorDef.flags = i == 0 ? MONITOR_PRIMARY : 0;
        monitors.push_back(monitorDef);
    }

    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu = {};
    resetGraphicsPdu.width = desktop.width();
    resetGraphicsPdu.height = desktop.height();
    resetGraphicsPdu.monitorCount = monitors.size();
    resetGraphicsPdu.monitorDefArray = monitors.data();
    UINT status = d->gfxContext->ResetGraphics(d->gfxContext.get(), &resetGraphicsPdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "ResetGraphics failed" << status << "for desktop" << desktop;
        return;
    }
    d->desktop = desktop;
    qCDebug(KRDP) << "Reset graphics to" << desktop.size() << "with" << monitors.size() << "monitors";

    // Recreate the surfaces of all monitors we already know the size of, the
    // ones that did not trigger this need to be painted again.
    for (std::size_t i = 0; i < d->outputs.size(); ++i) {
        if (!d->outputs[i].size.isValid() || !createSurface(i)) {
            continue;
        }
        if (int(i) != monitor) {
            QMetaObject::invokeMethod(
                this,
                [this, i]() {
                    refreshMonitor(i);
                },
                Qt::QueuedConnection);
        }
    }
}

bool VideoStream::hasInFlightCapacity() const
{
    std::lock_guard lock(d->pendingFramesMutex);
    return d->pendingFrames.size() < d->inFlightLimit.load();
}

void VideoStream::sendFrame(const VideoFrame &frame)
{
    // Keep coded packets intact when the socket is congested. FreeRDP's
    // bounded in-flight window and the encoder's single unsent packet apply
    // backpressure; dropping a reference frame here corrupts the stream and
    // provokes repeated, larger key frames.
    if (!d->gfxContext || !d->capsConfirmed) {
        return;
    }

    std::lock_guard lock(d->outputsMutex);
    if (frame.monitor < 0 || frame.monitor >= int(d->outputs.size())) {
        return;
    }
    auto &output = d->outputs.at(frame.monitor);
    output.size = frame.size;

    // A different desktop size needs a reset of the whole graphics pipeline,
    // otherwise only the surface of this monitor needs to change.
    const QRect desktop = d->desktopRect();
    if (d->pendingReset || desktop != d->desktop) {
        d->pendingReset = false;
        performReset(desktop, frame.monitor);
    } else if (output.surface.size != frame.size || output.surface.id == 0) {
        createSurface(frame.monitor);
    }

    if (!frame.data.isEmpty()) {
        const auto action = output.surface.gate.submit(frame.isKeyFrame);
        if (action != VideoFrameGate::Action::Send) {
            if (action == VideoFrameGate::Action::RequestKeyFrame) {
                qCDebug(KRDP) << "Waiting for key frame on surface" << output.surface.id << "monitor" << frame.monitor;
                requestKeyFrame(frame.monitor);
            }
            return;
        }
        // Sent as what it was encoded to, rather than by the encoding mode: a
        // packet taken from the queue just before AV1 fell back to H.264 is
        // still AV1, which the client decodes with the capabilities confirmed.
        sendFrameVideo(frame, output.surface, frame.isAv1 ? RDPGFX_CODECID_AV1 : RDPGFX_CODECID_AVC420);
    } else if (d->activeEncodingMode == EncodingMode::Progressive) {
        sendFrameProgressive(frame, output.surface);
    }
}

void VideoStream::sendFrameVideo(const VideoFrame &frame, const Surface &surface, uint16_t codecId)
{
    if (frame.data.isEmpty()) {
        return;
    }

    if (surface.id == 0) {
        qCWarning(KRDP) << "No graphics surface available for video frame submission";
        return;
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.insert(frameId, clk::steady_clock::now());
    }

    RDPGFX_START_FRAME_PDU startFramePdu = {};
    RDPGFX_END_FRAME_PDU endFramePdu = {};

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    RDPGFX_SURFACE_COMMAND surfaceCommand = {};
    surfaceCommand.surfaceId = surface.id;
    // AV1 is sent like AVC420.
    surfaceCommand.codecId = codecId;
    surfaceCommand.contextId = 0;
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    surfaceCommand.left = 0;
    surfaceCommand.top = 0;
    surfaceCommand.right = frame.size.width();
    surfaceCommand.bottom = frame.size.height();
    surfaceCommand.width = frame.size.width();
    surfaceCommand.height = frame.size.height();
    surfaceCommand.length = 0;
    surfaceCommand.data = nullptr;

    RDPGFX_AVC420_BITMAP_STREAM avcStream = {};
    surfaceCommand.extra = &avcStream;

    avcStream.data = (BYTE *)frame.data.data();
    avcStream.length = frame.data.length();

    RECTANGLE_16 regionRect = {};
    regionRect.left = 0;
    regionRect.top = 0;
    regionRect.right = frame.size.width();
    regionRect.bottom = frame.size.height();
    RDPGFX_H264_QUANT_QUALITY quality = {};
    quality.qp = 22;
    quality.p = 0;
    quality.qualityVal = 100;
    avcStream.meta.numRegionRects = 1;
    avcStream.meta.regionRects = &regionRect;
    avcStream.meta.quantQualityVals = &quality;

    // Starting and ending the frame in the same message as the frame itself
    // means one write to the channel instead of three.
    const UINT status = d->gfxContext->SurfaceFrameCommand(d->gfxContext.get(), &surfaceCommand, &startFramePdu, &endFramePdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "SurfaceFrameCommand failed" << status << "frameId" << frameId << "surface" << surface.id << "encodedBytes" << frame.data.size();
        // The client will never acknowledge it.
        unregisterFrame(frameId);
    }

    if (status == CHANNEL_RC_OK) {
        ++d->sentFrames;
        d->sentBytes += frame.data.size();
        if (frame.isKeyFrame) {
            ++d->sentKeyFrames;
        }
    }
    d->session->networkDetection()->stopBandwidthMeasure();
}

void VideoStream::sendFrameProgressive(const VideoFrame &frame, const Surface &surface)
{
    if (frame.image.isNull()) {
        return;
    }

    if (surface.id == 0) {
        qCWarning(KRDP) << "No graphics surface available for progressive frame submission";
        return;
    }

    // The codec reads the common formats as they are.
    QImage image = frame.image;
    auto pixelFormat = toPixelFormat(image.format());
    if (!pixelFormat) {
        image = image.convertToFormat(QImage::Format_RGB32);
        pixelFormat = toPixelFormat(image.format());
    }
    const QRect frameRect(QPoint(0, 0), image.size());
    auto invalidRegion = toRegion16(frame.damage, frameRect);
    if (!invalidRegion) {
        qCWarning(KRDP) << "Failed to build invalid region for progressive frame";
        return;
    }

    BYTE *encodedData = nullptr;
    UINT32 encodedSize = 0;
    const UINT32 rectCount = region16_n_rects(&*invalidRegion);
    const int compressionStatus = progressive_compress(d->progressive.get(),
                                                       image.constBits(),
                                                       image.sizeInBytes(),
                                                       *pixelFormat,
                                                       image.width(),
                                                       image.height(),
                                                       image.bytesPerLine(),
                                                       &*invalidRegion,
                                                       &encodedData,
                                                       &encodedSize);
    if (compressionStatus < 0 || !encodedData || encodedSize == 0) {
        region16_uninit(&*invalidRegion);
        qCWarning(KRDP) << "Failed to compress progressive frame"
                        << "status" << compressionStatus << "rects" << rectCount << "size" << frame.size;
        return;
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.insert(frameId, clk::steady_clock::now());
    }

    RDPGFX_START_FRAME_PDU startFramePdu = {};
    RDPGFX_END_FRAME_PDU endFramePdu = {};

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    const RECTANGLE_16 *extents = region16_extents(&*invalidRegion);

    RDPGFX_SURFACE_COMMAND surfaceCommand = {};
    surfaceCommand.surfaceId = surface.id;
    surfaceCommand.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
    surfaceCommand.contextId = surface.codecContextId;
    surfaceCommand.format = PIXEL_FORMAT_BGRX32;
    surfaceCommand.left = extents->left;
    surfaceCommand.top = extents->top;
    surfaceCommand.right = extents->right;
    surfaceCommand.bottom = extents->bottom;
    surfaceCommand.width = frame.size.width();
    surfaceCommand.height = frame.size.height();
    surfaceCommand.length = encodedSize;
    surfaceCommand.data = encodedData;
    surfaceCommand.extra = nullptr;

    const UINT status = d->gfxContext->SurfaceFrameCommand(d->gfxContext.get(), &surfaceCommand, &startFramePdu, &endFramePdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "SurfaceFrameCommand failed" << status << "frameId" << frameId << "surface" << surface.id << "encodedBytes" << encodedSize
                        << "damageRects" << rectCount;
        // The client will never acknowledge it.
        unregisterFrame(frameId);
    }

    d->session->networkDetection()->stopBandwidthMeasure();
    region16_uninit(&*invalidRegion);
}

void VideoStream::updateRequestedFrameRate()
{
    auto rtt = std::max(clk::duration_cast<clk::milliseconds>(d->session->networkDetection()->averageRTT()), clk::milliseconds(1));
    const auto acknowledgementTime = std::max(rtt, clk::duration_cast<clk::milliseconds>(clk::microseconds(d->acknowledgementUs.load())));
    const auto pacing = videoPacing(acknowledgementTime, d->maximumFrameRate);
    d->inFlightLimit = pacing.inFlightLimit;
    wakeFrameSubmission();
    auto now = clk::steady_clock::now();
    if (now - d->lastStatistics >= clk::seconds(5)) {
        const double elapsed = clk::duration<double>(now - d->lastStatistics).count();
        qsizetype pending;
        qsizetype queued;
        {
            std::lock_guard lock(d->pendingFramesMutex);
            pending = d->pendingFrames.size();
        }
        {
            std::lock_guard lock(d->frameQueueMutex);
            queued = d->frameQueue.size();
        }
        qCInfo(KRDP) << "Video stats: requested FPS" << d->requestedFrameRate.load() << "sent FPS" << d->sentFrames.exchange(0) / elapsed << "ack FPS"
                     << d->acknowledgedFrames.exchange(0) / elapsed << "Mbit/s" << d->sentBytes.exchange(0) * 8.0 / elapsed / 1000000.0 << "key frames"
                     << d->sentKeyFrames.exchange(0) << "RTT ms" << rtt.count() << "ack ms" << acknowledgementTime.count() << "pending" << pending << "limit"
                     << pacing.inFlightLimit << "queued" << queued;
        d->lastStatistics = now;
    }

    FrameRateEstimate estimate;
    estimate.timeStamp = now;
    estimate.estimate = pacing.frameRate;
    d->frameRateEstimates.append(estimate);

    if (now - d->lastFrameRateEstimation < FrameRateEstimateAveragePeriod) {
        return;
    }

    d->lastFrameRateEstimation = now;

    d->frameRateEstimates.erase(std::remove_if(d->frameRateEstimates.begin(),
                                               d->frameRateEstimates.end(),
                                               [now](const auto &estimate) {
                                                   return (now - estimate.timeStamp) > FrameRateEstimateAveragePeriod;
                                               }),
                                d->frameRateEstimates.cend());

    auto sum = std::accumulate(d->frameRateEstimates.cbegin(), d->frameRateEstimates.cend(), 0, [](int acc, const auto &estimate) {
        return acc + estimate.estimate;
    });
    auto average = qreal(sum) / d->frameRateEstimates.size();

    const int frameRate = std::clamp(qRound(average), 1, d->maximumFrameRate);
    const int maximumFrameRate = d->maximumFrameRate;

    // Every change renegotiates the PipeWire streams, which holds back frames
    // for a moment. Small changes are not worth that, except to get back to
    // the maximum.
    const int currentFrameRate = d->requestedFrameRate;
    const bool significant = std::abs(frameRate - currentFrameRate) >= std::max(MinimumFrameRateChange, currentFrameRate / 10);
    if (frameRate != currentFrameRate && (significant || frameRate == maximumFrameRate)) {
        d->requestedFrameRate = frameRate;
        for (const auto &stream : d->streams) {
            if (stream->encodedStream) {
                stream->encodedStream->setMaxFramerate(frameRate, 1);
                stream->encodedStream->setMaxPendingFrames(frameRate);
            }
            if (stream->sourceStream) {
                stream->sourceStream->setMaxFramerate({static_cast<quint32>(frameRate), 1});
            }
        }
    }
}
}

#include "moc_VideoStream.cpp"
