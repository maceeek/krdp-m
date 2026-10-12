// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RdpConnection.h"
#include "VirtualDisplayManager.h"
#include <AbstractSession.h>
#include <KStatusNotifierItem>
#include <functional>
#include <vector>

#include <QObject>

namespace KRdp
{
class AbstractSession;
class Server;
class RdpConnection;
}

class SessionWrapper;
class LocalPresenceMonitor;
class QAction;

class SessionController : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.krdpserver.Displays")

public:
    enum class SessionType {
        Portal,
        Plasma,
    };

    SessionController(KRdp::Server *server, SessionType sessionType);
    ~SessionController() override;

    /**
     * Use a virtual monitor with a fixed initial size instead of the size the
     * client asks for.
     */
    void setVirtualMonitor(const KRdp::VirtualMonitor &vm);
    void setMonitorIndex(const std::optional<int> &index);
    void setQuality(const std::optional<int> &quality);
    void setVirtualDisplayMode(VirtualDisplayManager::Mode mode);
    void setVirtualDisplayMaximumSize(const QSize &size);
    void setLocalTakeoverPolicy(bool detectInput, bool lockScreen, bool disconnect);
    void setSNIStatus(const KRdp::RdpConnection::State state);
    void stopFromSNI();

    /**
     * Restore the physical outputs if we changed them, then call \p done.
     */
    void shutdown(const std::function<void()> &done);

public Q_SLOTS:
    /**
     * Give the physical monitors back to someone sitting at the computer.
     *
     * Exported on D-Bus for `krdpserver --return-displays`.
     */
    Q_SCRIPTABLE void ReturnLocalDisplays();

private:
    void onNewConnection(KRdp::RdpConnection *newConnection);
    void startSession(KRdp::RdpConnection *connection);
    void onLocalUserDetected(const QString &reason);
    bool usesVirtualDisplay() const;
    std::unique_ptr<KRdp::AbstractSession> makeSession(bool virtualDisplay);

    KRdp::Server *m_server = nullptr;
    SessionType m_sessionType;
    std::optional<int> m_monitorIndex;
    std::optional<int> m_quality;
    std::optional<KRdp::VirtualMonitor> m_virtualMonitor;

    VirtualDisplayManager *m_virtualDisplays = nullptr;
    LocalPresenceMonitor *m_localPresence = nullptr;
    bool m_lockOnTakeover = true;
    bool m_disconnectOnTakeover = false;

    std::vector<std::unique_ptr<SessionWrapper>> m_wrappers;

    KStatusNotifierItem *m_sni;
    QAction *m_returnDisplaysAction = nullptr;
};
