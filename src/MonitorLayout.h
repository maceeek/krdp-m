// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QDebug>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRect>

#include "krdp_export.h"

namespace KRdp
{

/**
 * A monitor of an RDP client.
 */
struct KRDP_EXPORT Monitor {
    /**
     * Geometry in pixels, in the coordinate space of the client's desktop.
     */
    QRect rect;
    /**
     * The desktop scale factor the client asked for, 1.0 for 100%.
     */
    qreal scale = 1.0;
    bool primary = false;

    bool operator==(const Monitor &other) const = default;
};

/**
 * The monitors of an RDP client.
 *
 * A normalized layout has its bounding rectangle at the origin, which is the
 * coordinate space of the client's framebuffer, and the primary monitor first.
 */
using MonitorLayout = QList<Monitor>;

/**
 * The rectangle containing all monitors.
 */
KRDP_EXPORT QRect boundingRect(const MonitorLayout &layout);

/**
 * Move the layout so its bounding rectangle starts at the origin and put the
 * primary monitor first. Marks the first monitor as primary if none is.
 */
KRDP_EXPORT MonitorLayout normalized(const MonitorLayout &layout);

/**
 * Positions of the monitors in logical coordinates, i.e. with their scale
 * applied, keeping monitors that touch in pixels touching.
 */
KRDP_EXPORT QList<QPoint> logicalPositions(const MonitorLayout &layout);

/**
 * Index of the monitor containing \p position, or of the closest one.
 */
KRDP_EXPORT int monitorAt(const MonitorLayout &layout, const QPointF &position);

}

KRDP_EXPORT QDebug operator<<(QDebug debug, const KRdp::Monitor &monitor);
