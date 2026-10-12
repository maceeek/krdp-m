// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>

#include "krdp_export.h"

namespace KRdp
{

/**
 * H.264 encoder using NVENC on NVIDIA GPUs, through FFmpeg.
 *
 * KPipeWire only knows VAAPI for hardware encoding, which the NVIDIA driver
 * does not provide, so on NVIDIA GPUs KRdp would encode in software. This
 * encodes raw frames from a PipeWireSourceStream instead.
 *
 * Frames are encoded on a thread of their own. When frames come in faster
 * than they can be encoded only the latest one is kept. Every frame is
 * encoded as a whole into the same Baseline profile, full range BT.601
 * stream KPipeWire produces for KRdp.
 */
class KRDP_EXPORT NvencEncoder : public QObject
{
    Q_OBJECT

public:
    /**
     * \param codecName The FFmpeg encoder to use, only changed for testing.
     */
    explicit NvencEncoder(const QByteArray &codecName = QByteArrayLiteral("h264_nvenc"));
    ~NvencEncoder() override;

    /**
     * Whether NVENC can be tried on this system: FFmpeg was built with it and
     * the NVIDIA driver is loaded. If it still does not work, failed() is
     * emitted once the first frame is encoded.
     *
     * Can be turned off by setting KRDP_DISABLE_NVENC=1.
     */
    static bool isAvailable();

    /**
     * Set the quality from 0 to 100. When it changes, the next frame is a
     * key frame.
     */
    void setQuality(quint8 quality);

    /**
     * Encode \p image, replacing a frame that is still waiting to be encoded.
     */
    void encode(const QImage &image);

    /**
     * Encode the next frame as a key frame. Without a new frame the last
     * frame is encoded again.
     */
    void requestKeyFrame();

    /**
     * A frame of \p size was encoded. Emitted from the encoding thread.
     */
    Q_SIGNAL void packetReady(const QByteArray &data, bool isKeyFrame, const QSize &size);
    /**
     * The encoder could not be set up or stopped working, nothing will be
     * encoded anymore. Emitted from the encoding thread.
     */
    Q_SIGNAL void failed();

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
