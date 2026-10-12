// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QObject>
#include <QPoint>
#include <QRegion>
#include <QSize>

#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <freerdp/server/rdpgfx.h>

#include "MonitorLayout.h"
#include "VideoFrame.h"
#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;
struct Surface;

/**
 * A class that encapsulates an RdpGfx video stream.
 *
 * Video streaming is done using the "RDP Graphics Pipeline" protocol
 * extension which allows using h264 as the codec for the video stream.
 * However, this protocol extension is fairly complex to setup and use.
 *
 * VideoStream makes sure to handle most of the complexity of the RdpGfx
 * protocol like ensuring the client knows the right resolution and a
 * surface at the right size. It also takes care of sending the frames,
 * using a separate thread for a submission queue.
 *
 * VideoStream is managed by Session. Each session will have one instance
 * of this class.
 */
class KRDP_EXPORT VideoStream : public QObject
{
    Q_OBJECT

public:
    enum class EncodingMode {
        H264,
        Progressive,
    };

    explicit VideoStream(RdpConnection *session);
    ~VideoStream() override;

    static bool h264Disabled();

    bool initialize();
    void close();
    Q_SIGNAL void closed();
    /**
     * The size of the first monitor changed.
     */
    Q_SIGNAL void sizeChanged(const QSize &size);
    Q_SIGNAL void monitorSizeChanged(int monitor, const QSize &size);
    Q_SIGNAL void cursorChanged(const PipeWireCursor &cursor);

    /**
     * Queue a frame to be sent to the client.
     *
     * This will add the provided frame to the queue of frames that should
     * be sent to the client.
     *
     * \param frame The frame to send.
     */
    void queueFrame(const VideoFrame &frame);

    /**
     * Indicate that the video state should be reset.
     *
     * This means the screen resolution and other information of the client
     * will be updated based on the current state of the VideoStream.
     */
    void reset();

    /**
     */
    bool enabled() const;
    void setEnabled(bool enabled);
    Q_SIGNAL void enabledChanged();
    void setStreamingEnabled(bool enabled);
    void setVideoQuality(quint8 quality);
    /**
     * Set the PipeWire node to stream from.
     *
     * \param fd A PipeWire remote to use, or -1 to use the default PipeWire
     *           instance. The VideoStream takes ownership of the fd.
     */
    void setPipeWireSource(quint32 nodeId, int fd = -1);
    /**
     * Set the PipeWire node to stream the monitor with index \p monitor from.
     */
    void setPipeWireSource(int monitor, quint32 nodeId, int fd = -1);

    /**
     * Set the monitors of the client.
     *
     * Every monitor gets its own surface placed at its position, with its
     * own PipeWire stream set with setPipeWireSource(). The layout must be
     * normalized. Without a layout there is a single monitor, sized to its
     * stream.
     */
    void setMonitorLayout(const MonitorLayout &layout);
    int monitorCount() const;

    bool openChannel();

private:
    friend BOOL gfxChannelIdAssigned(RdpgfxServerContext *, uint32_t);
    friend uint32_t gfxCapsAdvertise(RdpgfxServerContext *, const RDPGFX_CAPS_ADVERTISE_PDU *);
    friend uint32_t gfxFrameAcknowledge(RdpgfxServerContext *, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *);

    bool onChannelIdAssigned(uint32_t channelId);
    uint32_t onCapsAdvertise(const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise);
    uint32_t onFrameAcknowledge(const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge);

    void onPacketReceived(int monitor, const PipeWireEncodedStream::Packet &data);
    void onFrameReceived(int monitor, const PipeWireFrame &frame);
    void onCursorChanged(int monitor, const PipeWireCursor &cursor);
    void setActiveEncodingMode(EncodingMode mode);
    void createStream(int monitor);
    void setStreamSize(int monitor, const QSize &size);
    void restartEncoder(int monitor);
    /**
     * Have the NVENC encoder of \p monitor send a key frame. Without NVENC,
     * KPipeWire sends key frames regularly by itself.
     */
    void requestKeyFrame(int monitor);
    void refreshMonitor(int monitor);
    void destroySurface(Surface &surface);
    void destroySurfaces();
    bool createSurface(int monitor);
    void performReset(const QRect &desktop, int monitor);
    bool hasInFlightCapacity() const;
    void sendFrame(const VideoFrame &frame);
    void sendFrameH264(const VideoFrame &frame, const Surface &surface);
    void sendFrameProgressive(const VideoFrame &frame, const Surface &surface);

    void updateRequestedFrameRate();

    class Private;
    const std::unique_ptr<Private> d;
};

}
