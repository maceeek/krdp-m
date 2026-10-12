// SPDX-FileCopyrightText: 2025 David Edmundson <davidedmundson@kde.org>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QSize>

#include <freerdp/server/disp.h>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

/**
 * Implementation of the server side of the Display Control dynamic virtual
 * channel ([MS-RDPEDISP]).
 *
 * Clients use this channel to tell the server that they would like a
 * different desktop size, for example because the client window was resized
 * or the client went fullscreen on a different monitor.
 */
class KRDP_EXPORT DisplayControl : public QObject
{
    Q_OBJECT
public:
    explicit DisplayControl(RdpConnection *session);
    ~DisplayControl() override;

    /**
     * Open the channel and advertise our capabilities to the client.
     *
     * Failure is not fatal for the connection, it only means the client
     * cannot request a different desktop size.
     */
    bool initialize();
    void close();

Q_SIGNALS:
    /**
     * Emitted when the client requests a new monitor layout.
     *
     * Only the primary monitor of the client layout is used.
     *
     * \param size The requested size in physical pixels.
     * \param scale The requested desktop scale factor, 1.0 for 100%.
     *
     * \note This is emitted from the RDP connection thread.
     */
    void requestedLayoutChanged(const QSize &size, qreal scale);

private:
    friend UINT displayControlReceiveMonitorLayout(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu);

    RdpConnection *m_session = nullptr;
    DispServerContext *m_dispManager = nullptr;
    bool m_opened = false;
};

}
