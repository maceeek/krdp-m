// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <cstdlib>
#include <libxcvt/libxcvt.h>

#include <KConfigGroup>
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
    void testCustomResize_data();
    void testCustomResize();
    void testLocalTakeover();
    void testMonitorEnabledExternally();
    void testRestoreAfterCrash();
    void testRestoreUnusableLayout();
    void testReturnAfterDisconnect();
    void testSanitizeSize();
    void testSanitizedSizesMatchCvt();
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
    // Widths are rounded down for CVT, heights for the encoder.
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

void VirtualDisplayManagerTest::testCustomResize_data()
{
    QTest::addColumn<VirtualDisplayManager::Mode>("mode");
    QTest::addColumn<QSize>("requested");
    QTest::addColumn<QSize>("expected");

    QTest::newRow("exclusive-1650") << VirtualDisplayManager::Mode::Exclusive << QSize(1650, 1064) << QSize(1648, 1064);
    QTest::newRow("extend-1650") << VirtualDisplayManager::Mode::Extend << QSize(1650, 1064) << QSize(1648, 1064);
    QTest::newRow("exclusive-1366") << VirtualDisplayManager::Mode::Exclusive << QSize(1366, 768) << QSize(1366, 768);
    QTest::newRow("cvt-rounds-up") << VirtualDisplayManager::Mode::Exclusive << QSize(1360, 768) << QSize(1352, 768);
    QTest::newRow("odd-dimensions") << VirtualDisplayManager::Mode::Exclusive << QSize(1651, 1065) << QSize(1648, 1064);
}

void VirtualDisplayManagerTest::testCustomResize()
{
    QFETCH(VirtualDisplayManager::Mode, mode);
    QFETCH(QSize, requested);
    QFETCH(QSize, expected);

    VirtualDisplayManager manager;
    manager.setMode(mode);
    auto session = std::make_unique<FakeSession>(u"Virtual-krdp"_s);
    manager.addSession(session.get(), QSize(1280, 720), 1.0);
    QTRY_COMPARE(output(currentConfig(), u"Virtual-krdp"_s)->pos(), mode == VirtualDisplayManager::Mode::Exclusive ? QPoint() : QPoint(4480, 0));

    // Add a custom mode, switch to fullscreen, then return to the window.
    const QList<QSize> requests{requested, QSize(1920, 1200), requested};
    const QList<QSize> expectedSizes{expected, QSize(1920, 1200), expected};
    for (int i = 0; i < requests.size(); ++i) {
        manager.requestLayout(session.get(), requests[i], 1.0);
        QTRY_VERIFY(currentSize(output(currentConfig(), u"Virtual-krdp"_s)) == expectedSizes[i]
                    || (!output(currentConfig(), u"Virtual-krdp"_s)->customModes().isEmpty()
                        && output(currentConfig(), u"Virtual-krdp"_s)->customModes().first().size.height() == expectedSizes[i].height()));

        // The Fake backend only stores the request. Model KWin's publication
        // of a generated mode using the same CVT generator as KWin. Its
        // size and refresh rate may differ from the request. The old
        // mode stays active until the manager selects the new one.
        if (currentSize(output(currentConfig(), u"Virtual-krdp"_s)) != expectedSizes[i]) {
            auto config = currentConfig()->clone();
            auto virtualOutput = output(config, u"Virtual-krdp"_s);
            auto info = virtualOutput->customModes().first();
            const std::unique_ptr<libxcvt_mode_info, decltype(&std::free)> cvt(
                libxcvt_gen_mode_info(info.size.width(), info.size.height(), info.refreshRate, false, false),
                &std::free);
            QVERIFY(cvt);
            info.size = QSize(cvt->hdisplay, cvt->vdisplay);
            info.refreshRate = cvt->vrefresh;
            virtualOutput->setCustomModes({info});
            KScreen::ModePtr generated(new KScreen::Mode);
            generated->setId(QString::number(100 + i));
            generated->setSize(info.size);
            generated->setRefreshRate(info.refreshRate);
            auto modes = virtualOutput->modes();
            modes.insert(generated->id(), generated);
            virtualOutput->setModes(modes);
            // Publish the backend's newly available modes before validating
            // configurations which select one of them.
            KScreen::BackendManager::instance()->setConfig(config);
            auto operation = new KScreen::SetConfigOperation(config);
            QVERIFY(operation->exec());
        }

        QTRY_COMPARE_WITH_TIMEOUT(currentSize(output(currentConfig(), u"Virtual-krdp"_s)), expectedSizes[i], 3000);
        QCOMPARE(manager.isExclusive(), mode == VirtualDisplayManager::Mode::Exclusive);
        QCOMPARE(output(currentConfig(), u"eDP-1"_s)->isEnabled(), mode != VirtualDisplayManager::Mode::Exclusive);
        QCOMPARE(output(currentConfig(), u"HDMI-A-1"_s)->isEnabled(), mode != VirtualDisplayManager::Mode::Exclusive);
    }
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
    QCOMPARE(manager.sanitizeSize(QSize(1366, 767)), QSize(1360, 766));
    QCOMPARE(manager.sanitizeSize(QSize(1650, 1064)), QSize(1648, 1064));
    QCOMPARE(manager.sanitizeSize(QSize(1366, 768)), QSize(1366, 768));
    QCOMPARE(manager.sanitizeSize(QSize(1360, 768)), QSize(1352, 768));
    QCOMPARE(manager.sanitizeSize(QSize(100, 50)), QSize(200, 200));
    QCOMPARE(manager.sanitizeSize(QSize(7680, 4320)), QSize(4096, 4096));
    manager.setMaximumSize(QSize(2560, 1440));
    QCOMPARE(manager.sanitizeSize(QSize(3840, 2160)), QSize(2560, 1440));
    manager.setMaximumSize(QSize(1655, 1065));
    QCOMPARE(manager.sanitizeSize(QSize(1920, 1200)), QSize(1648, 1064));
    manager.setMaximumSize(QSize(1360, 768));
    QCOMPARE(manager.sanitizeSize(QSize(1920, 1080)), QSize(1352, 768));
}

void VirtualDisplayManagerTest::testSanitizedSizesMatchCvt()
{
    VirtualDisplayManager manager;
    manager.setMaximumSize(QSize(8192, 8192));
    for (int width = 200; width <= 8192; width += 2) {
        for (int height : {200, 768, 1064, 1200, 2160, 8192}) {
            const auto size = manager.sanitizeSize(QSize(width, height));
            const std::unique_ptr<libxcvt_mode_info, decltype(&std::free)> cvt(libxcvt_gen_mode_info(size.width(), size.height(), 60.0f, false, false),
                                                                               &std::free);
            QVERIFY(cvt);
            QCOMPARE(QSize(cvt->hdisplay, cvt->vdisplay), size);
            QVERIFY(size.width() >= 200 && size.width() <= width);
            QVERIFY(size.height() >= 200 && size.height() <= height);
            QCOMPARE(size.width() % 2, 0);
            QCOMPARE(size.height() % 2, 0);
            QCOMPARE(manager.sanitizeSize(size), size);
        }
    }
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
