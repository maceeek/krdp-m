// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <KSharedConfig>

#include <kscreen/backendmanager_p.h>
#include <kscreen/config.h>
#include <kscreen/getconfigoperation.h>
#include <kscreen/mode.h>
#include <kscreen/output.h>
#include <kscreen/setconfigoperation.h>

#include <AbstractSession.h>

#include "VirtualDisplayManager.h"

using namespace Qt::StringLiterals;

namespace
{
class FakeSession : public KRdp::AbstractSession
{
public:
    explicit FakeSession(const QString &outputName)
        : m_outputName(outputName)
    {
        setVirtualMonitor({u"krdp"_s, QSize(1280, 720), 1.0});
    }

    void start() override
    {
    }
    void sendEvent(const std::shared_ptr<QEvent> &) override
    {
    }
    void setClipboardData(std::unique_ptr<QMimeData>) override
    {
    }
    QString virtualOutputName() const override
    {
        return m_outputName;
    }

private:
    QString m_outputName;
};

KScreen::ConfigPtr currentConfig()
{
    auto operation = new KScreen::GetConfigOperation();
    operation->exec();
    return operation->config();
}

KScreen::OutputPtr output(const KScreen::ConfigPtr &config, const QString &name)
{
    const auto outputs = config->outputs();
    for (const auto &output : outputs) {
        if (output->name() == name) {
            return output;
        }
    }
    return {};
}

QSize currentSize(const KScreen::OutputPtr &output)
{
    return output->currentMode() ? output->currentMode()->size() : QSize();
}
}

class VirtualDisplayManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testExclusive();
    void testExtend();
    void testResize();
    void testLocalTakeover();
    void testMonitorEnabledExternally();
    void testRestoreAfterCrash();
    void testRestoreUnusableLayout();
    void testReturnAfterDisconnect();
    void testSanitizeSize();
    void testMonitorNames();
    void testMultipleMonitors_data();
    void testMultipleMonitors();
};

void VirtualDisplayManagerTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    qputenv("KSCREEN_BACKEND", "Fake");
    qputenv("KSCREEN_BACKEND_INPROCESS", "1");
    qputenv("KSCREEN_LOGGING", "false");
}

void VirtualDisplayManagerTest::init()
{
    // Start every test from the configuration in the json file.
    KScreen::BackendManager::instance()->shutdownBackend();
    KScreen::BackendManager::instance()->setBackendArgs({{u"TEST_DATA"_s, QFINDTESTDATA("data/exclusive.json")}});

    auto stateConfig = KSharedConfig::openStateConfig(u"krdp-serverstaterc"_s);
    stateConfig->deleteGroup(u"SavedDisplayLayout"_s);
    stateConfig->sync();
}

void VirtualDisplayManagerTest::cleanup()
{
    KScreen::BackendManager::instance()->shutdownBackend();
}

void VirtualDisplayManagerTest::testExclusive()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);

    QTRY_VERIFY(manager.isExclusive());
    QVERIFY(VirtualDisplayManager::hasSavedLayout());

    auto config = currentConfig();
    QVERIFY(!output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(!output(config, u"HDMI-A-1"_s)->isEnabled());
    auto virtualOutput = output(config, u"Virtual-krdp"_s);
    QVERIFY(virtualOutput->isEnabled());
    QCOMPARE(virtualOutput->pos(), QPoint(0, 0));
    QCOMPARE(virtualOutput->priority(), 1u);

    // The last session ending restores the physical outputs as they were.
    QSignalSpy exclusiveSpy(&manager, &VirtualDisplayManager::exclusiveChanged);
    QPointer<KRdp::AbstractSession> released = session.get();
    manager.removeSession(std::move(session));
    QTRY_COMPARE(exclusiveSpy.count(), 1);
    QCOMPARE(exclusiveSpy.first().first().toBool(), false);
    QVERIFY(!VirtualDisplayManager::hasSavedLayout());
    // The session (and so its virtual output) is only released afterwards.
    QTRY_VERIFY(!released);

    config = currentConfig();
    auto internal = output(config, u"eDP-1"_s);
    auto external = output(config, u"HDMI-A-1"_s);
    QVERIFY(internal->isEnabled());
    QVERIFY(external->isEnabled());
    QCOMPARE(internal->pos(), QPoint(0, 0));
    QCOMPARE(external->pos(), QPoint(1920, 0));
    QCOMPARE(currentSize(external), QSize(2560, 1440));
    QCOMPARE(internal->priority(), 1u);
    QCOMPARE(external->priority(), 2u);
}

void VirtualDisplayManagerTest::testExtend()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Extend);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1920, 1080), 1.0);

    // The virtual output gets resized, the physical outputs stay untouched.
    QTRY_COMPARE(currentSize(output(currentConfig(), u"Virtual-krdp"_s)), QSize(1920, 1080));
    auto config = currentConfig();
    QVERIFY(output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(output(config, u"HDMI-A-1"_s)->isEnabled());
    QVERIFY(!manager.isExclusive());
    QVERIFY(!VirtualDisplayManager::hasSavedLayout());
}

void VirtualDisplayManagerTest::testResize()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());

    // A mode the output already has gets selected.
    manager.requestLayout(session.get(), QSize(1920, 1080), 1.0);
    QTRY_COMPARE(currentSize(output(currentConfig(), u"Virtual-krdp"_s)), QSize(1920, 1080));

    // Otherwise a custom mode gets added, which KWin turns into a real mode.
    // Odd sizes are rounded down, as the encoder needs even sizes.
    manager.requestLayout(session.get(), QSize(1601, 901), 1.5);
    QTRY_VERIFY(!output(currentConfig(), u"Virtual-krdp"_s)->customModes().isEmpty());
    auto virtualOutput = output(currentConfig(), u"Virtual-krdp"_s);
    QCOMPARE(virtualOutput->customModes().first().size, QSize(1600, 900));
    QCOMPARE(virtualOutput->customModes().first().refreshRate, 60.0f);
    QTRY_COMPARE(output(currentConfig(), u"Virtual-krdp"_s)->scale(), 1.5);

    // Resizing must not give the physical outputs back.
    QVERIFY(manager.isExclusive());
    QVERIFY(!output(currentConfig(), u"eDP-1"_s)->isEnabled());
}

void VirtualDisplayManagerTest::testLocalTakeover()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());

    manager.returnToLocalDisplays();
    QTRY_VERIFY(!manager.isExclusive());
    QVERIFY(!VirtualDisplayManager::hasSavedLayout());

    // The remote session continues on its virtual monitor next to the
    // physical ones.
    auto config = currentConfig();
    QVERIFY(output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(output(config, u"HDMI-A-1"_s)->isEnabled());
    auto virtualOutput = output(config, u"Virtual-krdp"_s);
    QVERIFY(virtualOutput->isEnabled());
    QCOMPARE(virtualOutput->pos(), QPoint(1920 + 2560, 0));
    QCOMPARE(virtualOutput->priority(), 3u);

    // And does not take the physical outputs again when it resizes.
    manager.requestLayout(session.get(), QSize(1920, 1080), 1.0);
    QTRY_COMPARE(currentSize(output(currentConfig(), u"Virtual-krdp"_s)), QSize(1920, 1080));
    QTest::qWait(500);
    QVERIFY(!manager.isExclusive());
    QVERIFY(output(currentConfig(), u"eDP-1"_s)->isEnabled());

    // Once all sessions are gone, the next one is exclusive again.
    manager.removeSession(std::move(session));
    auto next = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(next.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());
}

void VirtualDisplayManagerTest::testMonitorEnabledExternally()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());

    // Someone at the computer turns a monitor on, e.g. by plugging it in or
    // with kscreen-doctor. We must not fight that.
    QTest::qWait(3500);
    auto config = currentConfig();
    output(config, u"HDMI-A-1"_s)->setEnabled(true);
    auto operation = new KScreen::SetConfigOperation(config);
    QVERIFY(operation->exec());

    QTRY_VERIFY(!manager.isExclusive());
    config = currentConfig();
    QVERIFY(output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(output(config, u"HDMI-A-1"_s)->isEnabled());
    QVERIFY(output(config, u"Virtual-krdp"_s)->isEnabled());
}

void VirtualDisplayManagerTest::testRestoreAfterCrash()
{
    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    {
        VirtualDisplayManager manager;
        manager.setMode(VirtualDisplayManager::Mode::Exclusive);
        manager.addSession(session.get(), QSize(1280, 720), 1.0);
        QTRY_VERIFY(manager.isExclusive());
        // Destroyed without restoring, like a crash would.
    }
    QVERIFY(VirtualDisplayManager::hasSavedLayout());
    QVERIFY(!output(currentConfig(), u"eDP-1"_s)->isEnabled());

    VirtualDisplayManager manager;
    QSignalSpy finishedSpy(&manager, &VirtualDisplayManager::restoreFinished);
    manager.restoreSavedLayout();
    QVERIFY(finishedSpy.wait());
    QCOMPARE(finishedSpy.first().first().toBool(), true);
    QVERIFY(!VirtualDisplayManager::hasSavedLayout());

    auto config = currentConfig();
    QVERIFY(output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(output(config, u"HDMI-A-1"_s)->isEnabled());
    QCOMPARE(output(config, u"HDMI-A-1"_s)->pos(), QPoint(1920, 0));

    // Nothing to do the second time.
    QSignalSpy secondSpy(&manager, &VirtualDisplayManager::restoreFinished);
    manager.restoreSavedLayout();
    QVERIFY(secondSpy.wait());
    QCOMPARE(secondSpy.first().first().toBool(), true);
}

void VirtualDisplayManagerTest::testRestoreUnusableLayout()
{
    // A layout saved while the physical outputs were already off, e.g. as KWin
    // turned them off when the virtual output appeared.
    auto config = currentConfig();
    output(config, u"eDP-1"_s)->setEnabled(false);
    output(config, u"HDMI-A-1"_s)->setEnabled(false);
    auto operation = new KScreen::SetConfigOperation(config);
    QVERIFY(operation->exec());

    auto group = KSharedConfig::openStateConfig(u"krdp-serverstaterc"_s)->group(u"SavedDisplayLayout"_s);
    group.writeEntry("Outputs", R"([{"name":"eDP-1","enabled":false},{"name":"HDMI-A-1","enabled":false}])"_ba);
    group.sync();

    // Falls back to enabling the physical outputs, and gets done with that.
    VirtualDisplayManager manager;
    QSignalSpy finishedSpy(&manager, &VirtualDisplayManager::restoreFinished);
    manager.restoreSavedLayout();
    QVERIFY(finishedSpy.wait());
    QCOMPARE(finishedSpy.first().first().toBool(), true);
    QVERIFY(!VirtualDisplayManager::hasSavedLayout());

    config = currentConfig();
    QVERIFY(output(config, u"eDP-1"_s)->isEnabled());
    QVERIFY(output(config, u"HDMI-A-1"_s)->isEnabled());
}

void VirtualDisplayManagerTest::testReturnAfterDisconnect()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);

    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());

    // Someone at the computer right after the last session ended.
    manager.removeSession(std::move(session));
    manager.returnToLocalDisplays();
    QTRY_VERIFY(!manager.isExclusive());
    QTRY_VERIFY(!VirtualDisplayManager::hasSavedLayout());
    QVERIFY(output(currentConfig(), u"eDP-1"_s)->isEnabled());

    // That is not a takeover of the next session.
    auto next = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(next.get(), QSize(1280, 720), 1.0);
    QTRY_VERIFY(manager.isExclusive());
    QVERIFY(!output(currentConfig(), u"eDP-1"_s)->isEnabled());
}

void VirtualDisplayManagerTest::testSanitizeSize()
{
    VirtualDisplayManager manager;
    QCOMPARE(manager.sanitizeSize(QSize(1920, 1080)), QSize(1920, 1080));
    QCOMPARE(manager.sanitizeSize(QSize(1366, 767)), QSize(1366, 766));
    QCOMPARE(manager.sanitizeSize(QSize(100, 50)), QSize(200, 200));
    QCOMPARE(manager.sanitizeSize(QSize(7680, 4320)), QSize(4096, 4096));
    manager.setMaximumSize(QSize(2560, 1440));
    QCOMPARE(manager.sanitizeSize(QSize(3840, 2160)), QSize(2560, 1440));
}

void VirtualDisplayManagerTest::testMonitorNames()
{
    VirtualDisplayManager manager;
    manager.setMode(VirtualDisplayManager::Mode::Exclusive);
    QCOMPARE(manager.allocateMonitorName(), u"krdp"_s);

    auto first = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(first.get(), QSize(1280, 720), 1.0);
    QCOMPARE(manager.allocateMonitorName(), u"krdp-2"_s);

    // Still taken while the session is kept alive for restoring the outputs.
    manager.removeSession(std::move(first));
    QCOMPARE(manager.allocateMonitorName(), u"krdp-2"_s);
    QTRY_COMPARE(manager.allocateMonitorName(), u"krdp"_s);
}

void VirtualDisplayManagerTest::testMultipleMonitors_data()
{
    QTest::addColumn<VirtualDisplayManager::Mode>("mode");
    // Where the monitors of the client start.
    QTest::addColumn<QPoint>("origin");

    QTest::newRow("exclusive") << VirtualDisplayManager::Mode::Exclusive << QPoint(0, 0);
    // Next to the physical monitor.
    QTest::newRow("extend") << VirtualDisplayManager::Mode::Extend << QPoint(1920, 0);
}

void VirtualDisplayManagerTest::testMultipleMonitors()
{
    QFETCH(VirtualDisplayManager::Mode, mode);
    QFETCH(QPoint, origin);

    KScreen::BackendManager::instance()->shutdownBackend();
    KScreen::BackendManager::instance()->setBackendArgs({{u"TEST_DATA"_s, QFINDTESTDATA("data/multimonitor.json")}});

    VirtualDisplayManager manager;
    manager.setMode(mode);

    // Two monitors of one client side by side.
    int client = 0;
    auto first = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    auto second = std::make_unique<FakeSession>(u"Virtual-krdp-2"_s);
    manager.addSession(first.get(), QSize(1280, 1024), 1.0, &client, QPoint(0, 0));
    manager.addSession(second.get(), QSize(1024, 768), 1.0, &client, QPoint(1280, 0));

    QTRY_COMPARE(output(currentConfig(), u"Virtual-krdp-2"_s)->pos(), origin + QPoint(1280, 0));
    auto config = currentConfig();
    QCOMPARE(output(config, u"Virtual-krdp"_s)->pos(), origin);
    QCOMPARE(output(config, u"eDP-1"_s)->isEnabled(), mode != VirtualDisplayManager::Mode::Exclusive);
    QVERIFY(output(config, u"Virtual-krdp"_s)->isEnabled());
    QVERIFY(output(config, u"Virtual-krdp-2"_s)->isEnabled());
    QVERIFY(output(config, u"Virtual-krdp"_s)->priority() < output(config, u"Virtual-krdp-2"_s)->priority());

    // The client moves its second monitor below the first one.
    manager.requestLayout(second.get(), QSize(1024, 768), 1.0, QPoint(0, 1024));
    QTRY_COMPARE(output(currentConfig(), u"Virtual-krdp-2"_s)->pos(), origin + QPoint(0, 1024));
    QCOMPARE(output(currentConfig(), u"Virtual-krdp"_s)->pos(), origin);

    manager.removeSession(std::move(first));
    manager.removeSession(std::move(second));
    QTRY_VERIFY(!VirtualDisplayManager::hasSavedLayout());
    QTRY_VERIFY(output(currentConfig(), u"eDP-1"_s)->isEnabled());
}

QTEST_GUILESS_MAIN(VirtualDisplayManagerTest)

#include "virtualdisplaymanagertest.moc"
