// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QElapsedTimer>
#include <QObject>

/**
 * Detects someone sitting at the computer while a remote session has
 * disabled the physical outputs.
 *
 * This detects local input: keyboard, mouse or touch input after a short
 * period without input that did not come from a remote client. It is a
 * heuristic, it can not detect local input while the remote user is active
 * at the same time.
 *
 * The reliable way is the global shortcut (Meta+Ctrl+Alt+R by default), which
 * runs `krdpserver --return-displays` as a separate process so it even works
 * when this process is not responding. See org.kde.krdpserver.returndisplays.desktop.
 */
class LocalPresenceMonitor : public QObject
{
    Q_OBJECT

public:
    explicit LocalPresenceMonitor(QObject *parent = nullptr);
    ~LocalPresenceMonitor() override;

    /**
     * Whether detecting local input is enabled.
     */
    void setInputDetectionEnabled(bool enabled);

    /**
     * Only detect local input while armed, i.e. while the physical outputs
     * are disabled.
     */
    void setArmed(bool armed);

    /**
     * Must be called for every input event received from a remote client,
     * so it is not mistaken for local input.
     */
    void noteRemoteInput();

    /**
     * Lock the screen using the session's screen locker.
     */
    static void lockScreen();

Q_SIGNALS:
    void localUserDetected(const QString &reason);

private:
    void updateIdleTimeout();

    bool m_inputDetectionEnabled = true;
    bool m_armed = false;
    int m_idleTimeoutId = -1;
    QElapsedTimer m_lastRemoteInput;
};
