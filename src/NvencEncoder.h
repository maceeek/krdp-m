// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include "FrameEncoder.h"

namespace KRdp
{

/**
 * H.264 encoder using NVENC on NVIDIA GPUs, through FFmpeg.
 *
 * KPipeWire only knows VAAPI for hardware encoding, which the NVIDIA driver
 * does not provide, so on NVIDIA GPUs KRdp would encode in software. This
 * encodes raw frames from a PipeWireSourceStream instead. Every frame is
 * encoded as a whole into the same Baseline profile, full range BT.601
 * stream KPipeWire produces for KRdp.
 */
class KRDP_EXPORT NvencEncoder : public FrameEncoder
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

protected:
    bool encodeImage(const QImage &image, bool keyFrame) override;
    void close() override;

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
