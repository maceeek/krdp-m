// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include "FrameEncoder.h"

namespace KRdp
{

/**
 * AV1 encoder using the GPU, through the AV1 codec of FreeRDP-M: NVENC on
 * NVIDIA GPUs, VAAPI on AMD and Intel GPUs.
 *
 * AV1 is an extension of FreeRDP (RDPGFX_CAPVERSION_FRDP_1), it is only used
 * for clients that advertise it, every other client gets H.264. Every frame
 * is encoded as a whole into a 4:2:0 stream, which the GPUs of clients can
 * decode, with the full range BT.709 colors of FreeRDP.
 */
class KRDP_EXPORT Av1Encoder : public FrameEncoder
{
    Q_OBJECT

public:
    Av1Encoder();
    ~Av1Encoder() override;

    /**
     * Whether a GPU of this computer encodes AV1. Without one AV1 is not
     * used, encoding it in software is too slow for a desktop.
     *
     * Can be turned off by setting KRDP_DISABLE_AV1=1.
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
