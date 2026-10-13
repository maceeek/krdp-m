// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "GfxCapabilities.h"

#include <QtGlobal>

#include "krdp_logging.h"

namespace KRdp
{

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
    case RDPGFX_CAPVERSION_FRDP_1:
        return "RDPGFX_CAPVERSION_FRDP_1";
    default:
        return "UNKNOWN_VERSION";
    }
}

std::optional<GfxCapabilities> chooseGfxCapabilities(const RDPGFX_CAPSET *capsSets, uint16_t count, bool h264, bool av1)
{
    const RDPGFX_CAPSET *newest = nullptr;
    const RDPGFX_CAPSET *freeRdp = nullptr;
    bool supportsH264 = false;

    qCDebug(KRDP) << "Received caps:";
    for (uint16_t i = 0; i < count; ++i) {
        const RDPGFX_CAPSET &set = capsSets[i];

        bool avcSupported = false;
        bool yuv420Supported = false;
        switch (set.version) {
        case RDPGFX_CAPVERSION_107:
        case RDPGFX_CAPVERSION_106:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_104:
            yuv420Supported = true;
            Q_FALLTHROUGH();
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_10:
            if (!(set.flags & RDPGFX_CAPS_FLAG_AVC_DISABLED)) {
                avcSupported = true;
            }
            break;
        case RDPGFX_CAPVERSION_81:
            if (set.flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
                avcSupported = true;
                yuv420Supported = true;
            }
            break;
        case RDPGFX_CAPVERSION_8:
            break;
        }

        qCDebug(KRDP) << " " << capVersionToString(set.version) << "flags:" << Qt::hex << set.flags << Qt::dec << "AVC:" << avcSupported
                      << "YUV420:" << yuv420Supported;

        supportsH264 |= avcSupported && yuv420Supported;
        if (set.version == RDPGFX_CAPVERSION_FRDP_1) {
            // Not a version of the protocol, it does not take part in choosing
            // the newest one.
            freeRdp = &set;
        } else if (!newest || set.version > newest->version) {
            newest = &set;
        }
    }

    GfxCapabilities result;
    if (av1 && freeRdp) {
        // FreeRDP clients decode H.264 with this capability set too, unless
        // they can't decode it at all.
        result.codec = GfxCodec::Av1;
        result.capsSet = *freeRdp;
        result.h264Supported = h264 && supportsH264 && !(freeRdp->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED);
        return result;
    }

    if (!newest) {
        newest = freeRdp;
    }
    if (!newest) {
        return std::nullopt;
    }
    result.capsSet = *newest;
    if (h264 && supportsH264) {
        result.codec = GfxCodec::H264;
        result.h264Supported = true;
    }
    return result;
}

}
