// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "MonitorLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace KRdp
{

QRect boundingRect(const MonitorLayout &layout)
{
    QRect result;
    for (const auto &monitor : layout) {
        result |= monitor.rect;
    }
    return result;
}

MonitorLayout normalized(const MonitorLayout &layout)
{
    MonitorLayout result = layout;
    if (result.isEmpty()) {
        return result;
    }

    const QPoint offset = boundingRect(result).topLeft();
    for (auto &monitor : result) {
        monitor.rect.translate(-offset);
    }

    auto primary = std::ranges::find_if(result, &Monitor::primary);
    if (primary == result.end()) {
        result.first().primary = true;
    } else if (primary != result.begin()) {
        std::rotate(result.begin(), primary, primary + 1);
    }
    // Only one primary.
    for (int i = 1; i < result.size(); ++i) {
        result[i].primary = false;
    }

    return result;
}

namespace
{
// Logical coordinates along one axis: a monitor that starts where another one
// ends in pixels starts where that one ends in logical coordinates as well.
QList<int> logicalAxis(const MonitorLayout &layout, int (QRect::*start)() const, int (QRect::*length)() const)
{
    QList<int> order(layout.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, {}, [&](int index) {
        return (layout[index].rect.*start)();
    });

    QList<int> result(layout.size(), 0);
    QList<bool> placed(layout.size(), false);
    for (int index : std::as_const(order)) {
        const auto &monitor = layout[index];
        const int pixelStart = (monitor.rect.*start)();
        const qreal scale = monitor.scale > 0 ? monitor.scale : 1.0;

        int position = std::lround(pixelStart / scale);
        for (int other = 0; other < layout.size(); ++other) {
            if (!placed[other]) {
                continue;
            }
            const auto &neighbour = layout[other];
            if ((neighbour.rect.*start)() + (neighbour.rect.*length)() == pixelStart) {
                const qreal neighbourScale = neighbour.scale > 0 ? neighbour.scale : 1.0;
                position = result[other] + std::lround((neighbour.rect.*length)() / neighbourScale);
                break;
            }
        }

        result[index] = position;
        placed[index] = true;
    }
    return result;
}
}

QList<QPoint> logicalPositions(const MonitorLayout &layout)
{
    const auto xs = logicalAxis(layout, &QRect::x, &QRect::width);
    const auto ys = logicalAxis(layout, &QRect::y, &QRect::height);

    QList<QPoint> result;
    result.reserve(layout.size());
    for (int i = 0; i < layout.size(); ++i) {
        result.append(QPoint(xs[i], ys[i]));
    }
    return result;
}

int monitorAt(const MonitorLayout &layout, const QPointF &position)
{
    int closest = 0;
    qreal closestDistance = std::numeric_limits<qreal>::max();
    for (int i = 0; i < layout.size(); ++i) {
        const QRectF rect = layout[i].rect;
        // Half open, a position on the edge between two monitors belongs to
        // the one to the right or below.
        if (position.x() >= rect.left() && position.x() < rect.left() + rect.width() && position.y() >= rect.top()
            && position.y() < rect.top() + rect.height()) {
            return i;
        }
        const qreal dx = std::max({rect.left() - position.x(), 0.0, position.x() - rect.right()});
        const qreal dy = std::max({rect.top() - position.y(), 0.0, position.y() - rect.bottom()});
        const qreal distance = dx * dx + dy * dy;
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = i;
        }
    }
    return closest;
}

}

QDebug operator<<(QDebug debug, const KRdp::Monitor &monitor)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Monitor(" << monitor.rect << ", scale " << monitor.scale << (monitor.primary ? ", primary)" : ")");
    return debug;
}
