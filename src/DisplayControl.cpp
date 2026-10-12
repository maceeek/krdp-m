// SPDX-FileCopyrightText: 2025 David Edmundson <davidedmundson@kde.org>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DisplayControl.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include <freerdp/peer.h>

#include "krdp_logging.h"

namespace KRdp
{

UINT displayControlReceiveMonitorLayout(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu)
{
    if (pdu->NumMonitors < 1 || !pdu->Monitors) {
        return CHANNEL_RC_BAD_CHANNEL;
    }

    MonitorLayout layout;
    for (UINT32 i = 0; i < pdu->NumMonitors; ++i) {
        const auto &monitor = pdu->Monitors[i];
        layout.append(Monitor{
            .rect = QRect(monitor.Left, monitor.Top, monitor.Width, monitor.Height),
            // A percentage in the range 100 - 500, 0 if unset.
            .scale = monitor.DesktopScaleFactor >= 100 && monitor.DesktopScaleFactor <= 500 ? monitor.DesktopScaleFactor / 100.0 : 1.0,
            .primary = (monitor.Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) != 0,
        });
    }
    layout = normalized(layout);

    qCDebug(KRDP) << "Client requested monitor layout" << layout;

    auto displayControl = static_cast<DisplayControl *>(context->custom);
    Q_EMIT displayControl->requestedLayoutChanged(layout);

    return CHANNEL_RC_OK;
}

DisplayControl::DisplayControl(RdpConnection *session)
    : m_session(session)
{
}

DisplayControl::~DisplayControl()
{
    close();
}

bool DisplayControl::initialize()
{
    // Only try once, a client that does not support the channel will not
    // start supporting it later on.
    if (m_dispManager) {
        return m_opened;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(m_session->rdpPeer()->context);

    m_dispManager = disp_server_context_new(peerContext->virtualChannelManager);
    if (!m_dispManager) {
        qCWarning(KRDP) << "Failed creating DisplayControl context";
        return false;
    }

    m_dispManager->rdpcontext = m_session->rdpPeer()->context;
    m_dispManager->custom = this;
    m_dispManager->DispMonitorLayout = displayControlReceiveMonitorLayout;

    m_dispManager->MaxNumMonitors = MaximumMonitors;
    m_dispManager->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
    m_dispManager->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;

    if (m_dispManager->Open(m_dispManager) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not open DisplayControl context";
        return false;
    }
    m_opened = true;

    if (m_dispManager->DisplayControlCaps(m_dispManager) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not advertise DisplayControl capabilities";
        return false;
    }

    qCDebug(KRDP) << "DisplayControl channel initialized";
    return true;
}

void DisplayControl::close()
{
    if (m_dispManager) {
        if (m_opened) {
            (void)m_dispManager->Close(m_dispManager);
            m_opened = false;
        }
        disp_server_context_free(m_dispManager);
        m_dispManager = nullptr;
    }
}

}

#include "moc_DisplayControl.cpp"
