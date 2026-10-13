// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QImage>

#include <freerdp/codec/color.h>

namespace KRdp
{

/**
 * The FreeRDP pixel format with the memory layout of \p format, if there is one.
 */
inline std::optional<UINT32> toPixelFormat(QImage::Format format)
{
    switch (format) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        if (Q_BYTE_ORDER == Q_LITTLE_ENDIAN) {
            return PIXEL_FORMAT_BGRX32;
        }
        return PIXEL_FORMAT_XRGB32;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return PIXEL_FORMAT_RGBX32;
    default:
        return std::nullopt;
    }
}

}
