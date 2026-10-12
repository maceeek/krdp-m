// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "LocalPresenceMonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDebug>

#include <KIdleTime>

using namespace Qt::StringLiterals;

namespace
{
// Input after being idle for this long is checked for being local.
constexpr int IdleTimeoutMs = 2000;
// Input within this time after remote input is attributed to the remote client.
constexpr int RemoteInputWindowMs = 1500;
}

LocalPresenceMonitor::LocalPresenceMonitor(QObject *parent)
    : QObject(parent)
{
    connect(KIdleTime::instance(), &KIdleTime::timeoutReached, this, [this](int identifier) {
        if (identifier == m_idleTimeoutId) {
            KIdleTime::instance()->catchNextResumeEvent();
        }
    });
    connect(KIdleTime::instance(), &KIdleTime::resumingFromIdle, this, [this]() {
        if (m_idleTimeoutId == -1) {
            return;
        }
        if (m_lastRemoteInput.isValid() && m_lastRemoteInput.elapsed() < RemoteInputWindowMs) {
            return;
        }
        Q_EMIT localUserDetected(u"local input"_s);
    });
}

LocalPresenceMonitor::~LocalPresenceMonitor()
{
    if (m_idleTimeoutId != -1) {
        KIdleTime::instance()->removeIdleTimeout(m_idleTimeoutId);
    }
}

void LocalPresenceMonitor::setInputDetectionEnabled(bool enabled)
{
    m_inputDetectionEnabled = enabled;
    updateIdleTimeout();
}

void LocalPresenceMonitor::setArmed(bool armed)
{
    m_armed = armed;
    updateIdleTimeout();
}

void LocalPresenceMonitor::noteRemoteInput()
{
    m_lastRemoteInput.start();
}

void LocalPresenceMonitor::updateIdleTimeout()
{
    const bool wanted = m_armed && m_inputDetectionEnabled;
    if (wanted && m_idleTimeoutId == -1) {
        m_idleTimeoutId = KIdleTime::instance()->addIdleTimeout(IdleTimeoutMs);
    } else if (!wanted && m_idleTimeoutId != -1) {
        KIdleTime::instance()->removeIdleTimeout(m_idleTimeoutId);
        KIdleTime::instance()->stopCatchingResumeEvent();
        m_idleTimeoutId = -1;
    }
}

void LocalPresenceMonitor::lockScreen()
{
    auto message = QDBusMessage::createMethodCall(u"org.freedesktop.ScreenSaver"_s, u"/ScreenSaver"_s, u"org.freedesktop.ScreenSaver"_s, u"Lock"_s);
    QDBusConnection::sessionBus().asyncCall(message);
}

#include "moc_LocalPresenceMonitor.cpp"
