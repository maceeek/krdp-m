// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

#include <QHash>

namespace KRdp
{

inline std::optional<uint16_t> nextRdpSequenceNumber(uint16_t &sequenceNumber, const QHash<uint16_t, std::chrono::system_clock::time_point> &pendingRequests)
{
    // RDP carries only 16 bits. Use the same number as the pending-request
    // key, including after wrapping, and don't reuse an outstanding request.
    for (uint32_t attempt = 0; attempt < std::numeric_limits<uint16_t>::max(); ++attempt) {
        if (++sequenceNumber == 0) {
            ++sequenceNumber;
        }
        if (!pendingRequests.contains(sequenceNumber)) {
            return sequenceNumber;
        }
    }

    // Lost responses may occupy every nonzero number. Skip the measurement
    // instead of spinning forever on the connection thread.
    return std::nullopt;
}

}
