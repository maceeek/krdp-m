// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "AbstractSession.h"
#include <unistd.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QMimeData>

#include <KSystemClipboard>

#include "krdp_logging.h"

namespace KRdp
{

class KRDP_NO_EXPORT AbstractSession::Private
{
public:
    std::optional<int> activeStream;
    std::optional<VirtualMonitor> virtualMonitor;
    bool started = false;
    QSize size;
    QSize logicalSize;
    quint32 nodeId = 0;
    int pipeWireFd = -1;

    // set before our own setMimeData(), so the resulting changed() signal isn't bounced back out
    bool ignoreNextSystemClipboardChange = false;
};

AbstractSession::AbstractSession()
    : QObject()
    , d(std::make_unique<Private>())
{
    // Sessions are also created without a GUI application, by tests.
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        return;
    }

    connect(KSystemClipboard::instance(), &KSystemClipboard::changed, this, [this](auto mode) {
        if (mode != QClipboard::Clipboard) {
            return;
        }
        if (d->ignoreNextSystemClipboardChange) {
            d->ignoreNextSystemClipboardChange = false;
            return;
        }
        // Only the session of the first monitor of a connection forwards the
        // clipboard, and a session can outlive its connection. Nobody would
        // take ownership of the data then.
        if (!isSignalConnected(QMetaMethod::fromSignal(&AbstractSession::clipboardDataChanged))) {
            return;
        }

        auto data = KSystemClipboard::instance()->mimeData(mode);
        if (!data) {
            return;
        }

        // text only, arbitrary MIME payloads can block on Wayland/XWayland targets (SAVE_TARGETS)
        const QString text = data->hasText() ? data->text() : QString();
        if (!data->hasText()) {
            qCDebug(KRDP) << "Ignoring non-text clipboard update with formats" << data->formats();
        }

        // KSystemClipboard keeps ownership of what it returns, copy into a new QMimeData
        auto newData = new QMimeData();
        if (!text.isEmpty()) {
            newData->setText(text);
        }

        Q_EMIT clipboardDataChanged(newData);
    });
}

AbstractSession::~AbstractSession()
{
    if (d->pipeWireFd >= 0) {
        close(d->pipeWireFd);
    }
}

void AbstractSession::setClipboardData(std::unique_ptr<QMimeData> data)
{
    d->ignoreNextSystemClipboardChange = true;
    if (data) {
        KSystemClipboard::instance()->setMimeData(data.release(), QClipboard::Clipboard);
    } else {
        KSystemClipboard::instance()->clear(QClipboard::Clipboard);
    }
}

QSize AbstractSession::logicalSize() const
{
    return d->logicalSize;
}

std::optional<int> AbstractSession::activeStream() const
{
    return d->activeStream;
}

std::optional<VirtualMonitor> AbstractSession::virtualMonitor() const
{
    return d->virtualMonitor;
}

void AbstractSession::setActiveStream(int stream)
{
    Q_ASSERT(!d->virtualMonitor);
    d->activeStream = stream;
}

void AbstractSession::setVirtualMonitor(const VirtualMonitor &virtualMonitor)
{
    Q_ASSERT(!d->activeStream.has_value());
    d->virtualMonitor = virtualMonitor;
}

QString AbstractSession::virtualOutputName() const
{
    return QString();
}

void AbstractSession::setVirtualMonitorScale(qreal scale)
{
    if (!d->virtualMonitor || scale <= 0) {
        return;
    }
    d->virtualMonitor->dpr = scale;
    if (d->size.isValid()) {
        d->logicalSize = (QSizeF(d->size) / scale).toSize();
    }
}

QPointF AbstractSession::mapToLogical(const QPointF &position, const QRectF &target) const
{
    if (d->size.isEmpty()) {
        return target.topLeft();
    }
    return QPointF{target.x() + (position.x() / d->size.width()) * target.width(), target.y() + (position.y() / d->size.height()) * target.height()};
}

bool AbstractSession::isStarted() const
{
    return d->started;
}

void AbstractSession::setSize(QSize size)
{
    d->size = size;
    // A virtual monitor changes size along with the stream, so its logical
    // size needs to follow.
    if (d->virtualMonitor && d->started && size.isValid() && d->virtualMonitor->dpr > 0) {
        d->logicalSize = (QSizeF(size) / d->virtualMonitor->dpr).toSize();
    }
}

void AbstractSession::setLogicalSize(QSize size)
{
    d->logicalSize = size;
}

QSize AbstractSession::size() const
{
    return d->size;
}

quint32 AbstractSession::nodeId() const
{
    return d->nodeId;
}

int AbstractSession::takePipeWireFd()
{
    return std::exchange(d->pipeWireFd, -1);
}

void AbstractSession::setNodeId(quint32 nodeId)
{
    d->nodeId = nodeId;
}

void AbstractSession::setPipeWireFd(int fd)
{
    if (d->pipeWireFd >= 0) {
        close(d->pipeWireFd);
    }
    d->pipeWireFd = fd;
}

void AbstractSession::setStarted(bool s)
{
    d->started = s;
    if (s) {
        Q_EMIT started();
    }
}

}

#include "AbstractSession.moc"
