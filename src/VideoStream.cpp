// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-graphics-pipeline.c from Gnome Remote
// Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoStream.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

#include <QDateTime>
#include <QQueue>
#include <QSet>

#include <DmaBufHandler>
#include <PipeWireEncodedStream>
#include <freerdp/codec/progressive.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/update.h>
#include <qassert.h>

#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

// Maximum number of frames to contain in the queue.
constexpr clk::system_clock::duration FrameRateEstimateAveragePeriod = clk::seconds(1);
constexpr qsizetype MaximumInFlightFrames = 2;
constexpr uint32_t ProgressiveCodecContextId = 1;
struct RdpCapsInformation {
    uint32_t version;
    RDPGFX_CAPSET capSet;
    bool avcSupported : 1 = false;
    bool yuv420Supported : 1 = false;
};

const char *capVersionToString(uint32_t version)
{
    switch (version) {
    case RDPGFX_CAPVERSION_107:
        return "RDPGFX_CAPVERSION_107";
    case RDPGFX_CAPVERSION_106:
        return "RDPGFX_CAPVERSION_106";
    case RDPGFX_CAPVERSION_105:
        return "RDPGFX_CAPVERSION_105";
    case RDPGFX_CAPVERSION_104:
        return "RDPGFX_CAPVERSION_104";
    case RDPGFX_CAPVERSION_103:
        return "RDPGFX_CAPVERSION_103";
    case RDPGFX_CAPVERSION_102:
        return "RDPGFX_CAPVERSION_102";
    case RDPGFX_CAPVERSION_101:
        return "RDPGFX_CAPVERSION_101";
    case RDPGFX_CAPVERSION_10:
        return "RDPGFX_CAPVERSION_10";
    case RDPGFX_CAPVERSION_81:
        return "RDPGFX_CAPVERSION_81";
    case RDPGFX_CAPVERSION_8:
        return "RDPGFX_CAPVERSION_8";
    default:
        return "UNKNOWN_VERSION";
    }
}

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
};

struct FrameRateEstimate {
    clk::system_clock::time_point timeStamp;
    int estimate = 0;
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
        quint32 nodeId = 0;
        int pipeWireFd = -1;
        QSize size;
        // Set when the source changed size and the encoder needs to be
        // restarted, packets from the old encoder are dropped until then.
        bool encoderRestartPending = false;
        // The latest raw frame, to repaint the surface after it was recreated.
        QImage lastImage;
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

    bool enabled = false;
    bool streamingEnabled = false;
    bool capsConfirmed = false;
    bool channelOpen = false;

    std::jthread frameSubmissionThread;
    std::mutex frameQueueMutex;

    QQueue<VideoFrame> frameQueue;
    QSet<uint32_t> pendingFrames;

    std::mutex pendingFramesMutex;

    int maximumFrameRate = 120;
    std::atomic_int requestedFrameRate = 60;
    QQueue<FrameRateEstimate> frameRateEstimates;
    clk::system_clock::time_point lastFrameRateEstimation;

    std::atomic_int encodedFrames = 0;
    std::atomic_int frameDelay = 0;
    bool initialized = false;
    quint8 quality = 100;
};

static QString encodingModeName(VideoStream::EncodingMode mode)
{
    switch (mode) {
    case VideoStream::EncodingMode::H264:
        return QStringLiteral("h264");
    case VideoStream::EncodingMode::Progressive:
        return QStringLiteral("progressive");
    }
    Q_UNREACHABLE();
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
        std::lock_guard lock(d->frameQueueMutex);
        d->frameQueue.clear();
    }

    d->activeEncodingMode = mode;

    for (std::size_t monitor = 0; monitor < d->streams.size(); ++monitor) {
        createStream(monitor);
    }
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
    stream.encoderRestartPending = false;

    if (!d->activeEncodingMode) {
        return;
    }

    if (*d->activeEncodingMode == EncodingMode::H264) {
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
        stream.sourceStream = std::make_unique<PipeWireSourceStream>();
        stream.sourceStream->setAllowDmaBuf(true);
        stream.sourceStream->setDamageEnabled(true);
        stream.sourceStream->setMaxFramerate({static_cast<quint32>(d->requestedFrameRate.load()), 1});
        connect(
            stream.sourceStream.get(),
            &PipeWireSourceStream::frameReceived,
            this,
            [this, monitor](const PipeWireFrame &frame) {
                onFrameReceived(monitor, frame);
                if (frame.cursor) {
                    onCursorChanged(monitor, *frame.cursor);
                }
            },
            Qt::QueuedConnection);
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
    if (monitor < 0 || monitor >= int(d->streams.size()) || d->activeEncodingMode != EncodingMode::H264) {
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

    if (d->activeEncodingMode == EncodingMode::H264) {
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
    while (d->outputs.size() > count) {
        destroySurface(d->outputs.back().surface);
        d->outputs.pop_back();
    }
    d->outputs.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        d->outputs[i].origin = i < std::size_t(layout.size()) ? layout[i].rect.topLeft() : QPoint();
        d->outputs[i].expectedSize = i < std::size_t(layout.size()) ? layout[i].rect.size() : QSize();
    }
    d->pendingReset = true;
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
            if (!hasInFlightCapacity() || !d->gfxContext || !d->capsConfirmed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            VideoFrame nextFrame;
            {
                std::unique_lock lock(d->frameQueueMutex);
                if (!d->frameQueue.isEmpty()) {
                    nextFrame = d->frameQueue.takeFirst();
                }
            }
            if (nextFrame.size.isEmpty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000) / d->requestedFrameRate.load());
                continue;
            }
            sendFrame(nextFrame);
        }
    });

    qCDebug(KRDP) << "Video stream initialized with H.264" << (h264Disabled() ? "disabled" : "enabled");

    return true;
}

void VideoStream::close()
{
    for (const auto &stream : d->streams) {
        if (stream->encodedStream) {
            stream->encodedStream->stop();
        }
        if (stream->sourceStream) {
            stream->sourceStream->setActive(false);
        }
    }
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
    }

    destroySurfaces();

    if (d->gfxContext) {
        if (d->channelOpen) {
            d->gfxContext->Close(d->gfxContext.get());
            d->channelOpen = false;
        }
        d->gfxContext.reset();
    }
    d->activeEncodingMode.reset();
    d->initialized = false;

    Q_EMIT closed();
}

void VideoStream::queueFrame(const KRdp::VideoFrame &frame)
{
    if (d->session->state() != RdpConnection::State::Streaming || !d->enabled) {
        return;
    }

    std::lock_guard lock(d->frameQueueMutex);
    if (d->activeEncodingMode == EncodingMode::H264) {
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

    auto capsSets = capsAdvertise->capsSets;
    auto count = capsAdvertise->capsSetCount;

    std::vector<RdpCapsInformation> capsInformation;
    capsInformation.reserve(count);

    qCDebug(KRDP) << "Received caps:";
    for (int i = 0; i < count; ++i) {
        auto set = capsSets[i];

        RdpCapsInformation caps;
        caps.version = set.version;
        caps.capSet = set;

        switch (caps.version) {
        case RDPGFX_CAPVERSION_107:
        case RDPGFX_CAPVERSION_106:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_104:
            caps.yuv420Supported = true;
            Q_FALLTHROUGH();
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_10:
            if (!(set.flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)) {
                caps.avcSupported = true;
            }
            break;
        case RDPGFX_CAPVERSION_81:
            if (set.flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
                caps.avcSupported = true;
                caps.yuv420Supported = true;
            }
            break;
        case RDPGFX_CAPVERSION_8:
            break;
        }

        qCDebug(KRDP) << " " << capVersionToString(caps.version) << "flags:" << Qt::hex << set.flags << Qt::dec << "AVC:" << caps.avcSupported
                      << "YUV420:" << caps.yuv420Supported;

        capsInformation.push_back(caps);
    }

    const bool supportsProgresive = !capsInformation.empty();

    const bool supportsH264 = std::any_of(capsInformation.begin(), capsInformation.end(), [](const RdpCapsInformation &caps) {
        return caps.avcSupported && caps.yuv420Supported;
    });

    EncodingMode negotiatedMode = EncodingMode::Progressive;
    if (!h264Disabled() && supportsH264) {
        negotiatedMode = EncodingMode::H264;
    } else if (!supportsProgresive) {
        qCWarning(KRDP) << "Client advertised no usable graphics capability sets";
        d->session->close(RdpConnection::CloseReason::VideoInitFailed);
        return CHANNEL_RC_INITIALIZATION_ERROR;
    }

    QMetaObject::invokeMethod(
        this,
        [this, negotiatedMode]() {
            setActiveEncodingMode(negotiatedMode);
        },
        Qt::BlockingQueuedConnection); // RDP callbacks are on the connection thread, VideoStream operates on the main thread
    qCDebug(KRDP) << "Selected encoding mode:" << encodingModeName(negotiatedMode);

    auto maxVersion = std::max_element(capsInformation.begin(), capsInformation.end(), [](const auto &first, const auto &second) {
        return first.version < second.version;
    });

    qCDebug(KRDP) << "Selected caps:" << capVersionToString(maxVersion->version);

    RDPGFX_CAPS_CONFIRM_PDU capsConfirmPdu;
    capsConfirmPdu.capsSet = &(maxVersion->capSet);
    const UINT status = d->gfxContext->CapsConfirm(d->gfxContext.get(), &capsConfirmPdu);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "CapsConfirm failed" << status;
        return status;
    }

    d->capsConfirmed = true;

    return CHANNEL_RC_OK;
}

uint32_t VideoStream::onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge)
{
    auto id = frameAcknowledge->frameId;

    std::lock_guard lock(d->pendingFramesMutex);

    auto itr = d->pendingFrames.constFind(id);
    if (itr == d->pendingFrames.cend()) {
        qCWarning(KRDP) << "Got frame acknowledge for an unknown frame";
        return CHANNEL_RC_OK;
    }

    d->frameDelay = d->encodedFrames - frameAcknowledge->totalFramesDecoded;
    d->pendingFrames.erase(itr);

    return CHANNEL_RC_OK;
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
}

void VideoStream::onFrameReceived(int monitor, const PipeWireFrame &data)
{
    VideoFrame frameData;
    frameData.monitor = monitor;

    frameData.size = data.dataFrame ? data.dataFrame->size : QSize(data.dmabuf ? data.dmabuf->width : 0, data.dmabuf ? data.dmabuf->height : 0);
    frameData.damage = data.damage.value_or(QRegion(QRect(QPoint(0, 0), frameData.size)));
    if (data.presentationTimestamp) {
        frameData.presentationTimeStamp = clk::system_clock::time_point(clk::duration_cast<clk::microseconds>(*data.presentationTimestamp));
    }

    if (data.dataFrame) {
        frameData.image = data.dataFrame->toImage().convertToFormat(QImage::Format_RGB32);
    } else if (data.dmabuf) {
        QImage image(frameData.size, QImage::Format_RGBA8888_Premultiplied);
        if (!d->dmaBufHandler.downloadFrame(image, data)) {
            qCWarning(KRDP) << "Failed to download DMA-BUF frame";
            return;
        }
        frameData.image = std::move(image);
    } else {
        qCWarning(KRDP) << "PipeWire frame did not contain usable image data";
        return;
    }

    if (monitor < int(d->streams.size())) {
        d->streams.at(monitor)->lastImage = frameData.image;
    }
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

    RDPGFX_CREATE_SURFACE_PDU createSurfacePdu;
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
    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurfaceToOutputPdu;
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

    RDPGFX_RESET_GRAPHICS_PDU resetGraphicsPdu;
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
    return d->pendingFrames.size() < MaximumInFlightFrames;
}

void VideoStream::sendFrame(const VideoFrame &frame)
{
    auto peer = d->session->rdpPeer();
    if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
        return;
    }

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

    if (d->activeEncodingMode == EncodingMode::H264) {
        sendFrameH264(frame, output.surface);
    } else if (d->activeEncodingMode == EncodingMode::Progressive) {
        sendFrameProgressive(frame, output.surface);
    }
}

void VideoStream::sendFrameH264(const VideoFrame &frame, const Surface &surface)
{
    if (frame.data.isEmpty()) {
        return;
    }

    if (surface.id == 0) {
        qCWarning(KRDP) << "No graphics surface available for H264 frame submission";
        return;
    }

    d->session->networkDetection()->startBandwidthMeasure();

    auto frameId = d->frameId++;

    d->encodedFrames++;

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.insert(frameId);
    }

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    RDPGFX_SURFACE_COMMAND surfaceCommand;
    surfaceCommand.surfaceId = surface.id;
    surfaceCommand.codecId = RDPGFX_CODECID_AVC420;
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

    RDPGFX_AVC420_BITMAP_STREAM avcStream;
    surfaceCommand.extra = &avcStream;

    avcStream.data = (BYTE *)frame.data.data();
    avcStream.length = frame.data.length();

    avcStream.meta.numRegionRects = 1;
    auto rects = std::make_unique<RECTANGLE_16[]>(1);
    rects[0].left = 0;
    rects[0].top = 0;
    rects[0].right = frame.size.width();
    rects[0].bottom = frame.size.height();
    avcStream.meta.regionRects = rects.get();
    auto qualities = std::make_unique<RDPGFX_H264_QUANT_QUALITY[]>(1);
    avcStream.meta.quantQualityVals = qualities.get();
    qualities[0].qp = 22;
    qualities[0].p = 0;
    qualities[0].qualityVal = 100;

    const UINT startStatus = d->gfxContext->StartFrame(d->gfxContext.get(), &startFramePdu);
    if (startStatus != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "StartFrame failed" << startStatus << "frameId" << frameId;
        d->session->networkDetection()->stopBandwidthMeasure();
        return;
    }

    const UINT commandStatus = d->gfxContext->SurfaceCommand(d->gfxContext.get(), &surfaceCommand);
    if (commandStatus != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "SurfaceCommand failed" << commandStatus << "frameId" << frameId << "surface" << surface.id << "encodedBytes"
                        << frame.data.size();
    }

    const UINT endStatus = d->gfxContext->EndFrame(d->gfxContext.get(), &endFramePdu);
    if (endStatus != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "EndFrame failed" << endStatus << "frameId" << frameId;
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

    QImage image = frame.image.convertToFormat(QImage::Format_RGB32);
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
                                                       PIXEL_FORMAT_BGRX32,
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

    d->encodedFrames++;

    {
        std::lock_guard lock(d->pendingFramesMutex);
        d->pendingFrames.insert(frameId);
    }

    RDPGFX_START_FRAME_PDU startFramePdu;
    RDPGFX_END_FRAME_PDU endFramePdu;

    auto now = QDateTime::currentDateTimeUtc().time();
    startFramePdu.timestamp = now.hour() << 22 | now.minute() << 16 | now.second() << 10 | now.msec();

    startFramePdu.frameId = frameId;
    endFramePdu.frameId = frameId;

    const RECTANGLE_16 *extents = region16_extents(&*invalidRegion);

    RDPGFX_SURFACE_COMMAND surfaceCommand;
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
    }

    d->session->networkDetection()->stopBandwidthMeasure();
    region16_uninit(&*invalidRegion);
}

void VideoStream::updateRequestedFrameRate()
{
    auto rtt = std::max(clk::duration_cast<clk::milliseconds>(d->session->networkDetection()->averageRTT()), clk::milliseconds(1));
    auto now = clk::system_clock::now();

    FrameRateEstimate estimate;
    estimate.timeStamp = now;
    estimate.estimate = std::min(int(clk::milliseconds(1000) / (rtt * std::max(d->frameDelay.load(), 1))), d->maximumFrameRate);
    d->frameRateEstimates.append(estimate);

    if (now - d->lastFrameRateEstimation < FrameRateEstimateAveragePeriod) {
        return;
    }

    d->lastFrameRateEstimation = now;

    d->frameRateEstimates.erase(std::remove_if(d->frameRateEstimates.begin(),
                                               d->frameRateEstimates.end(),
                                               [now](const auto &estimate) {
                                                   return (estimate.timeStamp - now) > FrameRateEstimateAveragePeriod;
                                               }),
                                d->frameRateEstimates.cend());

    auto sum = std::accumulate(d->frameRateEstimates.cbegin(), d->frameRateEstimates.cend(), 0, [](int acc, const auto &estimate) {
        return acc + estimate.estimate;
    });
    auto average = sum / d->frameRateEstimates.size();

    // we want some headroom so we can always clear our current load
    // and handle any other latency
    constexpr qreal targetFrameRateSaturation = 0.5;
    auto frameRate = std::max(1.0, average * targetFrameRateSaturation);

    if (frameRate != d->requestedFrameRate) {
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
