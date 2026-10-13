// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRegion>
#include <QSize>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

/**
 * A frame of compressed video data.
 */
struct VideoFrame {
    /**
     * The monitor this frame is for.
     */
    int monitor = 0;
    /**
     * The size of the frame, in pixels.
     */
    QSize size;
    /**
     * h264 compressed data in YUV420 color space, or AV1 if isAv1 is set.
     */
    QByteArray data;
    /**
     * Whether data is AV1 rather than H.264.
     */
    bool isAv1 = false;
    /**
     * Raw frame data for classic raster updates.
     */
    QImage image;
    /**
     * Area of the frame that was actually damaged.
     * TODO: Actually use this information.
     */
    QRegion damage;
    /**
     * Whether the packet contains all the information
     */
    bool isKeyFrame = false;
    /**
     * When was this frame presented.
     */
    std::chrono::system_clock::time_point presentationTimeStamp;
    /**
     * Released when the last copy of the frame is gone, i.e. it was sent or
     * dropped, to let the encoder know.
     */
    std::shared_ptr<void> sent;
};
}
