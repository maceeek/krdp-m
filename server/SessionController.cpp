// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SessionController.h"

#include <QAction>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QMenu>
#include <QTimer>

#include <KLocalizedString>

#include <PipeWireSourceStream>

#include <Clipboard.h>
#include <Cursor.h>
#include <DisplayControl.h>
#include <InputHandler.h>
#include <PortalSession.h>
#include <RdpConnection.h>
#include <Server.h>

#ifdef WITH_PLASMA_SESSION
#include <PlasmaScreencastV1Session.h>
#endif

#include "LocalPresenceMonitor.h"
#include "VideoStream.h"

using namespace Qt::StringLiterals;

class SessionWrapper : public QObject
{
    Q_OBJECT
public:
    SessionWrapper(KRdp::RdpConnection *conn, std::unique_ptr<KRdp::AbstractSession> &&sess, KStatusNotifierItem *sni)
        : session(std::move(sess))
        , connection(conn)
    {
        m_sni = sni;

        connect(session.get(), &KRdp::AbstractSession::error, this, &SessionWrapper::sessionError);
        connect(session.get(), &KRdp::AbstractSession::started, this, &SessionWrapper::onSessionStarted);
        connect(session.get(), &KRdp::AbstractSession::clipboardDataChanged, connection->clipboard(), &KRdp::Clipboard::setServerData);

        connect(connection->videoStream(), &KRdp::VideoStream::cursorChanged, this, &SessionWrapper::onCursorUpdate);
        connect(connection->videoStream(), &KRdp::VideoStream::sizeChanged, session.get(), &KRdp::AbstractSession::setSize);
        connect(connection->videoStream(), &KRdp::VideoStream::enabledChanged, this, &SessionWrapper::onVideoStreamEnabledChanged);
        connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, session.get(), &KRdp::AbstractSession::sendEvent);
        connect(connection->clipboard(), &KRdp::Clipboard::clientDataChanged, session.get(), [clipboard = connection->clipboard(), this]() {
            session->setClipboardData(clipboard->getClipboard());
        });

        connect(connection, &QObject::destroyed, this, &SessionWrapper::onConnectionDestroyed);
    }

    void onCursorUpdate(const PipeWireCursor &cursor)
    {
        if (!connection) {
            return;
        }

        KRdp::Cursor::CursorUpdate update;
        update.hotspot = cursor.hotspot;
        update.image = cursor.texture;
        connection->cursor()->update(update);
    }

    void onVideoStreamEnabledChanged()
    {
        connection->videoStream()->setStreamingEnabled(m_sessionStarted && connection->videoStream()->enabled());
    }

    void onSessionStarted()
    {
        m_sessionStarted = true;
        connection->videoStream()->setPipeWireSource(session->nodeId(), session->takePipeWireFd());
        connection->videoStream()->setStreamingEnabled(connection->videoStream()->enabled());
    }

    void onConnectionDestroyed()
    {
        Q_EMIT connectionDestroyed(this);
    }

    Q_SIGNAL void sessionError();
    Q_SIGNAL void connectionDestroyed(SessionWrapper *wrapper);

    std::unique_ptr<KRdp::AbstractSession> session;
    QPointer<KRdp::RdpConnection> connection;
    KStatusNotifierItem *m_sni;
    bool m_sessionStarted = false;
    bool usesVirtualDisplay = false;
};

SessionController::SessionController(KRdp::Server *server, SessionType sessionType)
    : m_server(server)
    , m_sessionType(sessionType)
{
    connect(m_server, &KRdp::Server::newConnectionCreated, this, &SessionController::onNewConnection);

    m_virtualDisplays = new VirtualDisplayManager(this);
    m_localPresence = new LocalPresenceMonitor(this);
    connect(m_localPresence, &LocalPresenceMonitor::localUserDetected, this, &SessionController::onLocalUserDetected);

    // Status notification item
    m_sni = new KStatusNotifierItem(u"krdpserver"_s, this);
    auto menu = new QMenu(u"quitMenu"_s);
    // Disable default quit button since it has confirmation dialog
    m_sni->setStandardActionsEnabled(false);
    m_sni->setTitle(i18n("RDP Server"));
    m_sni->setIconByName(u"preferences-system-network-remote"_s);
    m_sni->setStatus(KStatusNotifierItem::Passive);
    m_returnDisplaysAction = new QAction(i18n("Return Displays to This Computer"), menu);
    m_returnDisplaysAction->setIcon(QIcon::fromTheme(u"video-display"_s));
    m_returnDisplaysAction->setVisible(false);
    connect(m_returnDisplaysAction, &QAction::triggered, this, &SessionController::ReturnLocalDisplays);
    menu->addAction(m_returnDisplaysAction);
    auto quitAction = new QAction(i18n("Quit"), menu);
    quitAction->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));
    connect(quitAction, &QAction::triggered, this, &SessionController::stopFromSNI);
    menu->addAction(quitAction);
    m_sni->setContextMenu(menu);

    connect(m_virtualDisplays, &VirtualDisplayManager::exclusiveChanged, this, [this](bool exclusive) {
        m_localPresence->setArmed(exclusive);
        m_returnDisplaysAction->setVisible(exclusive);
    });
}

SessionController::~SessionController() noexcept
{
}

void SessionController::setMonitorIndex(const std::optional<int> &index)
{
    m_monitorIndex = index;
}

void SessionController::setVirtualMonitor(const KRdp::VirtualMonitor &virtualMonitor)
{
    m_virtualMonitor = virtualMonitor;
}

void SessionController::setQuality(const std::optional<int> &quality)
{
    m_quality = quality;
}

void SessionController::setVirtualDisplayMode(VirtualDisplayManager::Mode mode)
{
    m_virtualDisplays->setMode(mode);
}

void SessionController::setVirtualDisplayMaximumSize(const QSize &size)
{
    m_virtualDisplays->setMaximumSize(size);
}

void SessionController::setLocalTakeoverPolicy(bool detectInput, bool lockScreen, bool disconnect)
{
    m_localPresence->setInputDetectionEnabled(detectInput);
    m_lockOnTakeover = lockScreen;
    m_disconnectOnTakeover = disconnect;
}

bool SessionController::usesVirtualDisplay() const
{
    if (m_virtualMonitor) {
        return true;
    }
    // An explicitly selected monitor wins over the configured virtual display.
    return !m_monitorIndex && m_virtualDisplays->mode() != VirtualDisplayManager::Mode::Disabled;
}

void SessionController::onNewConnection(KRdp::RdpConnection *newConnection)
{
    // Only start streaming once the client has authenticated. This also is
    // the point where we know the desktop size the client would like.
    connect(newConnection, &KRdp::RdpConnection::stateChanged, this, [this, newConnection](KRdp::RdpConnection::State state) {
        if (state != KRdp::RdpConnection::State::Activated) {
            return;
        }
        // The connection gets activated again after a desktop resize.
        const bool started = std::ranges::any_of(m_wrappers, [newConnection](const auto &wrapper) {
            return wrapper->connection == newConnection;
        });
        if (!started) {
            startSession(newConnection);
        }
    });
}

void SessionController::startSession(KRdp::RdpConnection *connection)
{
    const bool virtualDisplay = usesVirtualDisplay();

    auto wrapper = std::make_unique<SessionWrapper>(connection, makeSession(virtualDisplay), m_sni);
    if (virtualDisplay) {
        KRdp::VirtualMonitor monitor;
        monitor.name = m_virtualDisplays->allocateMonitorName();
        if (m_virtualMonitor) {
            monitor.size = m_virtualMonitor->size;
            monitor.dpr = m_virtualMonitor->dpr;
        } else {
            monitor.size = connection->clientDesktopSize();
            monitor.dpr = connection->clientDesktopScale();
        }
        monitor.size = m_virtualDisplays->sanitizeSize(monitor.size);

        wrapper->session->setVirtualMonitor(monitor);
        wrapper->usesVirtualDisplay = true;
        m_virtualDisplays->addSession(wrapper->session.get(), monitor.size, monitor.dpr);

        if (!m_virtualMonitor) {
            auto session = wrapper->session.get();
            connect(connection->displayControl(), &KRdp::DisplayControl::requestedLayoutChanged, wrapper.get(), [this, session](const QSize &size, qreal scale) {
                m_virtualDisplays->requestLayout(session, size, scale);
            });
        }
    } else if (m_monitorIndex) {
        wrapper->session->setActiveStream(*m_monitorIndex);
    }

    connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, m_localPresence, &LocalPresenceMonitor::noteRemoteInput);

    wrapper->connection->videoStream()->setVideoQuality(m_quality.value());
    wrapper->session->start();

    connect(wrapper.get(), &SessionWrapper::connectionDestroyed, this, [this](SessionWrapper *wrapper) {
        auto it = std::ranges::find_if(m_wrappers, [wrapper](const auto &entry) {
            return entry.get() == wrapper;
        });
        if (it == m_wrappers.end()) {
            return;
        }

        auto owned = std::move(*it);
        m_wrappers.erase(it);

        if (owned->usesVirtualDisplay) {
            // Keeps the virtual monitor around until the physical monitors
            // are back, so the desktop always has somewhere to go.
            m_virtualDisplays->removeSession(std::move(owned->session));
        }
        // We are called from a signal of the wrapper.
        owned.release()->deleteLater();
    });

    connect(wrapper.get(), &SessionWrapper::sessionError, this, [connection] {
        connection->close(KRdp::RdpConnection::CloseReason::None);
    });

    m_wrappers.push_back(std::move(wrapper));
}

void SessionController::ReturnLocalDisplays()
{
    onLocalUserDetected(u"request"_s);
}

void SessionController::onLocalUserDetected(const QString &reason)
{
    if (!m_virtualDisplays->isExclusive() && !VirtualDisplayManager::hasSavedLayout()) {
        return;
    }

    qInfo() << "Someone is at the computer (" << reason << "), returning the physical monitors";

    if (m_lockOnTakeover) {
        // Lock first, so whoever is at the computer does not get to see the
        // desktop of the remote user once the monitors come back.
        LocalPresenceMonitor::lockScreen();
        QTimer::singleShot(500, m_virtualDisplays, &VirtualDisplayManager::returnToLocalDisplays);
    } else {
        m_virtualDisplays->returnToLocalDisplays();
    }

    if (m_disconnectOnTakeover) {
        for (const auto &wrapper : m_wrappers) {
            if (wrapper->connection) {
                wrapper->connection->close(KRdp::RdpConnection::CloseReason::None);
            }
        }
    }
}

void SessionController::shutdown(const std::function<void()> &done)
{
    if (!VirtualDisplayManager::hasSavedLayout()) {
        done();
        return;
    }

    auto called = std::make_shared<bool>(false);
    auto finish = [called, done]() {
        if (!*called) {
            *called = true;
            done();
        }
    };
    connect(m_virtualDisplays, &VirtualDisplayManager::restoreFinished, this, finish);
    QTimer::singleShot(4000, this, finish);
    m_virtualDisplays->restoreSavedLayout();
}

void SessionController::stopFromSNI()
{
    // Uses dbus to stop the server service, like in the KCM
    // This kills all krdpserver instances, like a "panic button"
    QDBusInterface unit(u"org.freedesktop.systemd1"_s,
                        u"/org/freedesktop/systemd1/unit/app_2dorg_2ekde_2ekrdpserver_2eservice"_s,
                        u"org.freedesktop.systemd1.Unit"_s);

    unit.asyncCall(u"Stop"_s);
    shutdown([]() {
        QCoreApplication::quit();
    });
}

std::unique_ptr<KRdp::AbstractSession> SessionController::makeSession(bool virtualDisplay)
{
    Q_UNUSED(virtualDisplay)
#ifdef WITH_PLASMA_SESSION
    // The Plasma protocols let us name the virtual output and create it at
    // the right size directly, without a portal dialog.
    if (m_sessionType == SessionType::Plasma || (virtualDisplay && KRdp::PlasmaScreencastV1Session::isAvailable())) {
        return std::make_unique<KRdp::PlasmaScreencastV1Session>();
    } else
#endif
    {
        return std::make_unique<KRdp::PortalSession>();
    }
}

#include "SessionController.moc"
