// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <vector>

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>

#include <kscreen/types.h>

namespace KRdp
{
class AbstractSession;
}

/**
 * Manages the compositor outputs used for virtual monitors of RDP sessions.
 *
 * Every session that streams a virtual monitor is registered here. The
 * manager then makes sure the output matches what the client asked for
 * (resolution and scale) and, in exclusive mode, disables all other
 * outputs (the physical monitors) for as long as a remote session is active.
 *
 * Disabling the physical outputs always happens only once the virtual output
 * exists, and the previous layout of the physical outputs is saved to disk
 * before changing anything, so it can be restored when:
 *
 * - the last remote session ends,
 * - someone at the computer takes over (see LocalPresenceMonitor),
 * - krdpserver starts after a crash, or is run with --restore-displays.
 *
 * Note that KWin also restores the layout it stored for the physical outputs
 * by itself as soon as the virtual output disappears, which happens when
 * krdpserver exits for any reason. The saved layout here is a second line of
 * defence on top of that.
 *
 * All changes go through a single reconcile() step that compares the current
 * output configuration with the desired one and applies the difference, so
 * the manager converges on the right state regardless of the order in which
 * outputs appear or change.
 */
class VirtualDisplayManager : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        Disabled, ///< Stream the existing physical outputs, no virtual monitor.
        Extend, ///< Add a virtual monitor next to the physical outputs.
        Exclusive, ///< Add a virtual monitor and disable the physical outputs while connected.
    };
    Q_ENUM(Mode)

    explicit VirtualDisplayManager(QObject *parent = nullptr);
    ~VirtualDisplayManager() override;

    Mode mode() const;
    void setMode(Mode mode);

    void setMaximumSize(const QSize &size);

    /**
     * Clamp a size requested by a client to a size we can create.
     */
    QSize sanitizeSize(const QSize &size) const;

    /**
     * A name for a new virtual monitor that is not used by another session.
     */
    QString allocateMonitorName() const;

    /**
     * Register a session that uses a virtual monitor.
     *
     * \param size The size the virtual monitor should have, in pixels.
     * \param scale The scale the virtual monitor should have.
     */
    void addSession(KRdp::AbstractSession *session, const QSize &size, qreal scale);

    /**
     * Request a new size and scale for the virtual monitor of a session.
     *
     * Requests are debounced, as clients send a lot of them while their
     * window is being resized.
     */
    void requestLayout(KRdp::AbstractSession *session, const QSize &size, qreal scale);

    /**
     * Unregister a session.
     *
     * The manager takes ownership of the session and keeps it alive until
     * the physical outputs have been restored, so the virtual output only
     * disappears once there is something else to show the desktop on.
     */
    void removeSession(std::unique_ptr<KRdp::AbstractSession> session);

    /**
     * Whether the physical outputs are currently disabled by us.
     */
    bool isExclusive() const;

    /**
     * Give the physical outputs back to someone sitting at the computer.
     *
     * The physical outputs are restored and stay enabled until the last
     * remote session has ended, remote sessions continue on their virtual
     * monitors next to them.
     */
    void returnToLocalDisplays();

    /**
     * Restore a layout that was saved by a previous instance that did not
     * exit cleanly. Emits restoreFinished() when done.
     */
    void restoreSavedLayout();

    /**
     * Whether a saved layout of the physical outputs exists.
     */
    static bool hasSavedLayout();

Q_SIGNALS:
    void exclusiveChanged(bool exclusive);
    void restoreFinished(bool success);

private:
    struct Display {
        QPointer<KRdp::AbstractSession> session;
        QString outputName;
        bool prefixMatch = false;
        int outputId = -1;
        QSize size;
        qreal scale = 1.0;
        QSize pendingSize;
        qreal pendingScale = 1.0;
        std::unique_ptr<QTimer> debounce;
    };

    void fetchConfig();
    void scheduleReconcile(int delay = 0);
    void reconcile();
    void apply(const KScreen::ConfigPtr &config);
    void resolveOutputs();
    void releaseSessions();
    void setExclusive(bool exclusive);

    bool wantsExclusive() const;
    bool applyExclusive(const KScreen::ConfigPtr &config);
    bool applySavedLayout(const KScreen::ConfigPtr &config);
    bool applyDisplayModes(const KScreen::ConfigPtr &config);
    /**
     * Enable our virtual outputs and place them next to each other starting
     * at \p x, their priorities are appended to \p priorities.
     */
    void placeVirtualOutputs(const KScreen::ConfigPtr &config, int x, QMap<KScreen::OutputPtr, uint32_t> &priorities, bool &changed);
    void applyPriorities(const KScreen::ConfigPtr &config, QMap<KScreen::OutputPtr, uint32_t> &priorities, bool &changed);

    void saveLayout(const KScreen::ConfigPtr &config);
    void clearSavedLayout();

    /**
     * Whether the output is a virtual monitor of one of our sessions. In
     * exclusive mode, all other outputs are turned off.
     */
    bool isOurs(const KScreen::OutputPtr &output) const;
    /**
     * Whether the output is a monitor connected to the computer, as opposed
     * to a virtual output created by an application.
     */
    static bool isHardware(const KScreen::OutputPtr &output);

    Mode m_mode = Mode::Disabled;
    QSize m_maximumSize = QSize(4096, 4096);

    KScreen::ConfigPtr m_config;
    bool m_operationInFlight = false;
    int m_attempts = 0;
    QTimer m_reconcileTimer;
    QTimer m_releaseTimer;

    std::vector<std::unique_ptr<Display>> m_displays;
    std::vector<std::unique_ptr<KRdp::AbstractSession>> m_releasedSessions;

    // True from the moment we save the layout of the physical outputs until
    // it has been restored again.
    bool m_haveSavedLayout = false;
    bool m_exclusive = false;
    // Set when someone at the computer took over, cleared once all remote
    // sessions are gone.
    bool m_localTakeover = false;
    bool m_restoreRequested = false;
    QStringList m_physicalOutputsAtEntry;
    QElapsedTimer m_lastApply;
};
