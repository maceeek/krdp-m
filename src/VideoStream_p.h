// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace KRdp
{
// A newly created surface has no reference pictures. Request exactly one key
// frame and discard every dependent packet until that key frame arrives.
class VideoFrameGate
{
public:
    enum class Action {
        Send,
        RequestKeyFrame,
        Wait
    };

    Action submit(bool keyFrame)
    {
        if (keyFrame) {
            m_waiting = false;
        }
        if (!m_waiting) {
            return Action::Send;
        }
        if (!m_requested) {
            m_requested = true;
            return Action::RequestKeyFrame;
        }
        return Action::Wait;
    }

private:
    bool m_waiting = true;
    bool m_requested = false;
};

struct VideoPacing {
    int frameRate;
    int inFlightLimit;
};

inline VideoPacing videoPacing(std::chrono::milliseconds acknowledgementTime, int maximumFrameRate = 60)
{
    const double milliseconds = std::max(acknowledgementTime.count(), int64_t(1));
    // Enough pipelining for 60 Hz over a normal Wi-Fi RTT, bounded to six
    // pictures. RTT is a turnaround time, not a per-frame processing cost.
    const int window = std::clamp(int(std::ceil(std::min(milliseconds, 1000.0) * maximumFrameRate / 1000.0)) + 1, 2, 6);
    const int rate = std::clamp(int(0.9 * window * 1000.0 / milliseconds), 1, maximumFrameRate);
    return {rate, window};
}
}
