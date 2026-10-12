// SPDX-FileCopyrightText: 2025 David Edmundson <davidedmundson@kde.org>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QSize>

#include "MonitorLayout.h"

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
    /**
     * The maximum number of monitors a client can use, the limit of RDP.
     */
    static constexpr int MaximumMonitors = 16;

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
     * \param layout The requested monitors, normalized.
     *
     * \note This is emitted from the RDP connection thread.
     */
    void requestedLayoutChanged(const KRdp::MonitorLayout &layout);

private:
    friend UINT displayControlReceiveMonitorLayout(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu);

    RdpConnection *m_session = nullptr;
    DispServerContext *m_dispManager = nullptr;
    bool m_opened = false;
};

}
