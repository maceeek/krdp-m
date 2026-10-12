// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "VirtualDisplayManager.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRect>

#include <KConfigGroup>
#include <KSharedConfig>

#include <kscreen/config.h>
#include <kscreen/configmonitor.h>
#include <kscreen/getconfigoperation.h>
#include <kscreen/mode.h>
#include <kscreen/output.h>
#include <kscreen/setconfigoperation.h>

#include <AbstractSession.h>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(KRDP_DISPLAYS, "org.kde.krdp.displays", QtInfoMsg)

namespace
{
// KWin prefixes the names of all outputs created for screencasting.
const QString VirtualOutputPrefix = u"Virtual-"_s;
const QString MonitorNameBase = u"krdp"_s;

// How long to wait after the last layout request from a client before
// resizing, clients send many while their window is resized.
constexpr int LayoutDebounceMs = 400;
// Give up converging on a configuration after this many applies.
constexpr int MaxAttempts = 6;
// Keep sessions that ended alive for at most this long while waiting for
// the physical outputs to be restored.
constexpr int ReleaseTimeoutMs = 5000;
// Changes made to the outputs within this time after we applied a
// configuration are attributed to us.
constexpr int OwnChangeGraceMs = 3000;

constexpr float RefreshRate = 60.0;

KConfigGroup layoutGroup()
{
    return KSharedConfig::openStateConfig(u"krdp-serverstaterc"_s)->group(u"SavedDisplayLayout"_s);
}

KScreen::ModePtr findMode(const KScreen::OutputPtr &output, const QSize &size, float refreshRate)
{
    KScreen::ModePtr best;
    const auto modes = output->modes();
    for (const auto &mode : modes) {
        if (mode->size() != size) {
            continue;
        }
        if (!best || std::abs(mode->refreshRate() - refreshRate) < std::abs(best->refreshRate() - refreshRate)) {
            best = mode;
        }
    }
    return best;
}

bool sameScale(qreal a, qreal b)
{
    return std::abs(a - b) < 0.01;
}
}

VirtualDisplayManager::VirtualDisplayManager(QObject *parent)
    : QObject(parent)
{
    m_reconcileTimer.setSingleShot(true);
    connect(&m_reconcileTimer, &QTimer::timeout, this, &VirtualDisplayManager::reconcile);

    m_releaseTimer.setSingleShot(true);
    m_releaseTimer.setInterval(ReleaseTimeoutMs);
    connect(&m_releaseTimer, &QTimer::timeout, this, [this]() {
        if (!m_releasedSessions.empty()) {
            qCWarning(KRDP_DISPLAYS) << "Timed out waiting for the physical outputs to be restored";
            releaseSessions();
        }
    });

    m_haveSavedLayout = hasSavedLayout();

    connect(KScreen::ConfigMonitor::instance(), &KScreen::ConfigMonitor::configurationChanged, this, [this]() {
        scheduleReconcile(100);
    });

    fetchConfig();
}

VirtualDisplayManager::~VirtualDisplayManager()
{
    if (m_config) {
        KScreen::ConfigMonitor::instance()->removeConfig(m_config);
    }
}

VirtualDisplayManager::Mode VirtualDisplayManager::mode() const
{
    return m_mode;
}

void VirtualDisplayManager::setMode(Mode mode)
{
    m_mode = mode;
}

void VirtualDisplayManager::setMaximumSize(const QSize &size)
{
    if (size.isValid()) {
        m_maximumSize = size;
    }
}

QSize VirtualDisplayManager::sanitizeSize(const QSize &size) const
{
    // Limits from [MS-RDPEDISP], the encoder needs even dimensions.
    const int width = std::clamp(size.width(), 200, std::min(m_maximumSize.width(), 8192));
    const int height = std::clamp(size.height(), 200, std::min(m_maximumSize.height(), 8192));
    return QSize(width & ~1, height & ~1);
}

QString VirtualDisplayManager::allocateMonitorName() const
{
    for (int i = 1;; ++i) {
        const QString name = i == 1 ? MonitorNameBase : MonitorNameBase + u'-' + QString::number(i);
        const bool used = std::ranges::any_of(m_displays, [&name](const auto &display) {
            return display->outputName == VirtualOutputPrefix + name;
        });
        if (!used) {
            return name;
        }
    }
}

void VirtualDisplayManager::addSession(KRdp::AbstractSession *session, const QSize &size, qreal scale)
{
    auto display = std::make_unique<Display>();
    display->session = session;
    display->outputName = session->virtualOutputName();
    // Only the Plasma session knows the exact name of its output.
    display->prefixMatch = !display->outputName.startsWith(VirtualOutputPrefix + MonitorNameBase);
    display->size = sanitizeSize(size);
    display->scale = scale;
    display->debounce = std::make_unique<QTimer>();
    display->debounce->setSingleShot(true);
    display->debounce->setInterval(LayoutDebounceMs);

    auto rawDisplay = display.get();
    connect(display->debounce.get(), &QTimer::timeout, this, [this, rawDisplay]() {
        if (rawDisplay->pendingSize == rawDisplay->size && sameScale(rawDisplay->pendingScale, rawDisplay->scale)) {
            return;
        }
        qCInfo(KRDP_DISPLAYS) << "Resizing virtual monitor" << rawDisplay->outputName << "to" << rawDisplay->pendingSize << "scale" << rawDisplay->pendingScale;
        rawDisplay->size = rawDisplay->pendingSize;
        rawDisplay->scale = rawDisplay->pendingScale;
        m_attempts = 0;
        scheduleReconcile();
    });

    qCInfo(KRDP_DISPLAYS) << "Virtual monitor" << display->outputName << "requested at" << display->size << "scale" << scale << "mode" << m_mode;

    m_displays.push_back(std::move(display));
    m_attempts = 0;
    scheduleReconcile();
}

void VirtualDisplayManager::requestLayout(KRdp::AbstractSession *session, const QSize &size, qreal scale)
{
    auto it = std::ranges::find_if(m_displays, [session](const auto &display) {
        return display->session == session;
    });
    if (it == m_displays.end()) {
        return;
    }

    (*it)->pendingSize = sanitizeSize(size);
    (*it)->pendingScale = scale > 0 ? scale : 1.0;
    (*it)->debounce->start();
}

void VirtualDisplayManager::removeSession(std::unique_ptr<KRdp::AbstractSession> session)
{
    std::erase_if(m_displays, [&session](const auto &display) {
        return !display->session || display->session == session.get();
    });

    if (m_displays.empty()) {
        // The next remote session gets the exclusive mode again.
        m_localTakeover = false;
    }

    if (session) {
        m_releasedSessions.push_back(std::move(session));
        m_releaseTimer.start();
    }

    m_attempts = 0;
    scheduleReconcile();
}

bool VirtualDisplayManager::isExclusive() const
{
    return m_exclusive;
}

void VirtualDisplayManager::returnToLocalDisplays()
{
    if (!m_haveSavedLayout && !m_exclusive) {
        return;
    }

    qCInfo(KRDP_DISPLAYS) << "Returning the physical outputs to the local user";
    m_localTakeover = true;
    m_attempts = 0;
    scheduleReconcile();
}

void VirtualDisplayManager::restoreSavedLayout()
{
    m_restoreRequested = true;
    m_attempts = 0;
    scheduleReconcile();
}

bool VirtualDisplayManager::hasSavedLayout()
{
    return layoutGroup().hasKey("Outputs");
}

void VirtualDisplayManager::fetchConfig()
{
    auto operation = new KScreen::GetConfigOperation();
    connect(operation, &KScreen::ConfigOperation::finished, this, [this](KScreen::ConfigOperation *operation) {
        if (operation->hasError()) {
            qCWarning(KRDP_DISPLAYS) << "Could not read the output configuration:" << operation->errorString();
            if (m_restoreRequested) {
                m_restoreRequested = false;
                Q_EMIT restoreFinished(false);
            }
            return;
        }

        m_config = qobject_cast<KScreen::GetConfigOperation *>(operation)->config();
        KScreen::ConfigMonitor::instance()->addConfig(m_config);
        scheduleReconcile();
    });
}

void VirtualDisplayManager::scheduleReconcile(int delay)
{
    if (!m_reconcileTimer.isActive() || delay < m_reconcileTimer.remainingTime()) {
        m_reconcileTimer.start(delay);
    }
}

void VirtualDisplayManager::resolveOutputs()
{
    const auto outputs = m_config->outputs();

    for (const auto &display : m_displays) {
        if (display->outputId != -1) {
            auto output = m_config->output(display->outputId);
            if (output && output->isConnected() && output->name().startsWith(display->outputName)) {
                continue;
            }
            display->outputId = -1;
        }

        for (const auto &output : outputs) {
            if (!output->isConnected()) {
                continue;
            }
            const bool matches = display->prefixMatch ? output->name().startsWith(display->outputName) : output->name() == display->outputName;
            if (!matches) {
                continue;
            }
            const bool claimed = std::ranges::any_of(m_displays, [&output](const auto &other) {
                return other->outputId == output->id();
            });
            if (claimed) {
                continue;
            }

            qCInfo(KRDP_DISPLAYS) << "Found virtual output" << output->name();
            display->outputId = output->id();
            break;
        }
    }
}

bool VirtualDisplayManager::wantsExclusive() const
{
    if (m_mode != Mode::Exclusive || m_localTakeover || m_restoreRequested) {
        return false;
    }

    // Only ever disable the physical outputs once there is a virtual one to
    // show the desktop on.
    return std::ranges::any_of(m_displays, [](const auto &display) {
        return display->outputId != -1;
    });
}

void VirtualDisplayManager::reconcile()
{
    if (!m_config) {
        return;
    }
    if (m_operationInFlight) {
        // Gets rescheduled once the operation is done.
        return;
    }

    resolveOutputs();

    auto config = m_config->clone();
    bool changed = false;

    if (wantsExclusive()) {
        changed = applyExclusive(config);
    } else if (m_haveSavedLayout) {
        changed = applySavedLayout(config);
    }

    if (applyDisplayModes(config)) {
        changed = true;
    }

    if (!changed) {
        m_attempts = 0;

        if (wantsExclusive()) {
            setExclusive(true);
        } else if (m_haveSavedLayout) {
            qCInfo(KRDP_DISPLAYS) << "Physical outputs restored";
            clearSavedLayout();
        }

        if (!m_haveSavedLayout) {
            setExclusive(false);
            releaseSessions();
            if (m_restoreRequested) {
                m_restoreRequested = false;
                Q_EMIT restoreFinished(true);
            }
        }
        return;
    }

    if (++m_attempts > MaxAttempts) {
        qCWarning(KRDP_DISPLAYS) << "Giving up applying the output configuration after" << MaxAttempts << "attempts";
        releaseSessions();
        if (m_restoreRequested) {
            m_restoreRequested = false;
            Q_EMIT restoreFinished(false);
        }
        return;
    }

    apply(config);
}

bool VirtualDisplayManager::applyExclusive(const KScreen::ConfigPtr &config)
{
    bool changed = false;
    const auto outputs = config->outputs();

    if (m_exclusive && m_lastApply.isValid() && m_lastApply.elapsed() > OwnChangeGraceMs) {
        // A physical output got enabled or plugged in while we are exclusive,
        // that means someone is at the computer.
        for (const auto &output : outputs) {
            if (!isPhysical(output) || !output->isConnected()) {
                continue;
            }
            if (output->isEnabled() || !m_physicalOutputsAtEntry.contains(output->name())) {
                qCInfo(KRDP_DISPLAYS) << "Physical output" << output->name() << "changed while exclusive, returning to local displays";
                m_localTakeover = true;
                return m_haveSavedLayout ? applySavedLayout(config) : false;
            }
        }
    }

    if (!m_haveSavedLayout) {
        saveLayout(config);
    }

    for (const auto &output : outputs) {
        if (isPhysical(output) && output->isConnected() && output->isEnabled()) {
            qCInfo(KRDP_DISPLAYS) << "Disabling physical output" << output->name();
            output->setEnabled(false);
            changed = true;
        }
    }

    placeVirtualOutputs(config, 0, 1, changed);
    return changed;
}

bool VirtualDisplayManager::applySavedLayout(const KScreen::ConfigPtr &config)
{
    const auto document = QJsonDocument::fromJson(layoutGroup().readEntry("Outputs", QByteArray()));
    const auto saved = document.array();

    bool changed = false;
    bool anyEnabled = false;
    QRect physicalGeometry;

    for (const auto &value : saved) {
        const auto entry = value.toObject();
        const QString name = entry[u"name"_s].toString();

        const auto outputs = config->outputs();
        auto it = std::ranges::find_if(outputs, [&name](const auto &output) {
            return output->name() == name;
        });
        if (it == outputs.end() || !(*it)->isConnected()) {
            continue;
        }
        const auto &output = *it;

        const bool enabled = entry[u"enabled"_s].toBool();
        if (output->isEnabled() != enabled) {
            output->setEnabled(enabled);
            changed = true;
        }
        if (!enabled) {
            continue;
        }
        anyEnabled = true;

        const QPoint pos(entry[u"x"_s].toInt(), entry[u"y"_s].toInt());
        if (output->pos() != pos) {
            output->setPos(pos);
            changed = true;
        }

        const QSize size(entry[u"width"_s].toInt(), entry[u"height"_s].toInt());
        if (auto mode = findMode(output, size, entry[u"refreshRate"_s].toDouble()); mode && output->currentModeId() != mode->id()) {
            output->setCurrentModeId(mode->id());
            changed = true;
        }

        const qreal scale = entry[u"scale"_s].toDouble(1.0);
        if (!sameScale(output->scale(), scale)) {
            output->setScale(scale);
            changed = true;
        }

        const uint32_t priority = entry[u"priority"_s].toInt(1);
        if (output->priority() != priority) {
            config->setOutputPriority(output, priority);
            changed = true;
        }

        physicalGeometry |= QRect(pos, config->logicalSizeForOutputInt(*output));
    }

    if (!anyEnabled) {
        // The saved layout is unusable, e.g. the monitors were swapped while
        // we were connected. Fall back to enabling whatever is connected.
        for (const auto &output : config->outputs()) {
            if (isPhysical(output) && output->isConnected() && !output->isEnabled()) {
                qCInfo(KRDP_DISPLAYS) << "Enabling physical output" << output->name();
                output->setEnabled(true);
                changed = true;
            }
        }
    }

    // Remote sessions that are still going continue on their virtual
    // monitors, placed to the right of the physical ones.
    placeVirtualOutputs(config, physicalGeometry.isValid() ? physicalGeometry.right() + 1 : 0, saved.size() + 1, changed);

    return changed;
}

void VirtualDisplayManager::placeVirtualOutputs(const KScreen::ConfigPtr &config, int x, uint32_t priority, bool &changed)
{
    for (const auto &display : m_displays) {
        auto output = display->outputId != -1 ? config->output(display->outputId) : KScreen::OutputPtr();
        if (!output) {
            continue;
        }

        if (!output->isEnabled()) {
            output->setEnabled(true);
            changed = true;
        }
        if (output->pos() != QPoint(x, 0)) {
            output->setPos(QPoint(x, 0));
            changed = true;
        }
        if (output->priority() != priority) {
            config->setOutputPriority(output, priority);
            changed = true;
        }

        x += config->logicalSizeForOutputInt(*output).width();
        ++priority;
    }
}

bool VirtualDisplayManager::applyDisplayModes(const KScreen::ConfigPtr &config)
{
    bool changed = false;

    for (const auto &display : m_displays) {
        auto output = display->outputId != -1 ? config->output(display->outputId) : KScreen::OutputPtr();
        if (!output) {
            continue;
        }

        if (auto mode = findMode(output, display->size, RefreshRate)) {
            if (output->currentModeId() != mode->id()) {
                output->setCurrentModeId(mode->id());
                changed = true;
            }
        } else {
            // KWin lets us add arbitrary modes to virtual outputs. Once the
            // mode shows up in the output's mode list it gets selected above.
            const QList<KScreen::ModeInfo> customModes{KScreen::ModeInfo{
                .size = display->size,
                .refreshRate = RefreshRate,
                .flags = KScreen::ModeInfo::Flag::Custom,
            }};
            if (output->customModes() != customModes) {
                output->setCustomModes(customModes);
                changed = true;
            }
        }

        if (!sameScale(output->scale(), display->scale)) {
            output->setScale(display->scale);
            changed = true;
        }

        if (display->session) {
            display->session->setVirtualMonitorScale(display->scale);
        }
    }

    return changed;
}

void VirtualDisplayManager::apply(const KScreen::ConfigPtr &config)
{
    if (!KScreen::Config::canBeApplied(config)) {
        qCWarning(KRDP_DISPLAYS) << "Refusing to apply an invalid output configuration";
        releaseSessions();
        return;
    }

    m_operationInFlight = true;
    m_lastApply.start();

    auto operation = new KScreen::SetConfigOperation(config);
    connect(operation, &KScreen::ConfigOperation::finished, this, [this](KScreen::ConfigOperation *operation) {
        m_operationInFlight = false;
        m_lastApply.start();
        if (operation->hasError()) {
            qCWarning(KRDP_DISPLAYS) << "Applying the output configuration failed:" << operation->errorString();
        }
        // The config monitor updates m_config shortly after, check again
        // once that happened.
        scheduleReconcile(250);
    });
}

void VirtualDisplayManager::releaseSessions()
{
    m_releaseTimer.stop();
    m_releasedSessions.clear();
}

void VirtualDisplayManager::setExclusive(bool exclusive)
{
    if (m_exclusive == exclusive) {
        return;
    }

    m_exclusive = exclusive;
    qCInfo(KRDP_DISPLAYS) << (exclusive ? "Physical outputs disabled, remote session is exclusive" : "Physical outputs are active");
    Q_EMIT exclusiveChanged(exclusive);
}

void VirtualDisplayManager::saveLayout(const KScreen::ConfigPtr &config)
{
    QJsonArray outputs;
    m_physicalOutputsAtEntry.clear();

    for (const auto &output : config->outputs()) {
        if (!isPhysical(output) || !output->isConnected()) {
            continue;
        }

        m_physicalOutputsAtEntry.append(output->name());

        const auto mode = output->currentMode();
        outputs.append(QJsonObject{
            {u"name"_s, output->name()},
            {u"hash"_s, output->hashMd5()},
            {u"enabled"_s, output->isEnabled()},
            {u"x"_s, output->pos().x()},
            {u"y"_s, output->pos().y()},
            {u"width"_s, mode ? mode->size().width() : 0},
            {u"height"_s, mode ? mode->size().height() : 0},
            {u"refreshRate"_s, mode ? mode->refreshRate() : 0.0},
            {u"scale"_s, output->scale()},
            {u"priority"_s, int(output->priority())},
        });
    }

    auto group = layoutGroup();
    group.writeEntry("Outputs", QJsonDocument(outputs).toJson(QJsonDocument::Compact));
    group.sync();
    m_haveSavedLayout = true;

    qCInfo(KRDP_DISPLAYS) << "Saved layout of physical outputs" << m_physicalOutputsAtEntry;
}

void VirtualDisplayManager::clearSavedLayout()
{
    auto group = layoutGroup();
    group.deleteGroup();
    group.sync();
    m_haveSavedLayout = false;
    m_physicalOutputsAtEntry.clear();
}

bool VirtualDisplayManager::isPhysical(const KScreen::OutputPtr &output)
{
    return !output->name().startsWith(VirtualOutputPrefix);
}

#include "moc_VirtualDisplayManager.cpp"
