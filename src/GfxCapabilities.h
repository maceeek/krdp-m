// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <freerdp/channels/rdpgfx.h>

#include "krdp_export.h"

namespace KRdp
{

enum class GfxCodec {
    H264,
    Av1,
    Progressive,
};

struct GfxCapabilities {
    GfxCodec codec = GfxCodec::Progressive;
    /**
     * The capability set to confirm, one the client advertised.
     */
    RDPGFX_CAPSET capsSet = {};
    /**
     * Whether H.264 may be sent with capsSet, to fall back to when AV1 stops
     * working.
     */
    bool h264Supported = false;
};

/**
 * Choose the codec and the capability set to confirm from the \p count
 * capability sets a client advertised.
 *
 * AV1 is only chosen when \p av1 is true and the client advertised
 * RDPGFX_CAPVERSION_FRDP_1, which only FreeRDP clients built with AV1 do.
 * Otherwise H.264 is chosen when \p h264 is true and the client supports it,
 * with the newest capability set as before, so other clients see no
 * difference. Progressive is the fallback.
 *
 * Returns nothing if the client advertised no capability sets.
 */
KRDP_EXPORT std::optional<GfxCapabilities> chooseGfxCapabilities(const RDPGFX_CAPSET *capsSets, uint16_t count, bool h264, bool av1);

KRDP_EXPORT const char *capVersionToString(uint32_t version);

}
