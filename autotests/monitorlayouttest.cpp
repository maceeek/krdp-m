// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include <MonitorLayout.h>

using namespace KRdp;

class MonitorLayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNormalized();
    void testLogicalPositions_data();
    void testLogicalPositions();
    void testMonitorAt();
};

void MonitorLayoutTest::testNormalized()
{
    // RDP puts the primary monitor at the origin, others can be at negative
    // coordinates.
    const MonitorLayout layout{
        Monitor{.rect = QRect(-1920, 0, 1920, 1080)},
        Monitor{.rect = QRect(0, 0, 2560, 1440), .primary = true},
    };
    const auto result = normalized(layout);
    QCOMPARE(result.size(), 2);
    QVERIFY(result[0].primary);
    QCOMPARE(result[0].rect, QRect(1920, 0, 2560, 1440));
    QVERIFY(!result[1].primary);
    QCOMPARE(result[1].rect, QRect(0, 0, 1920, 1080));
    QCOMPARE(boundingRect(result), QRect(0, 0, 4480, 1440));

    // Without a primary the first one becomes primary.
    const auto noPrimary = normalized({Monitor{.rect = QRect(10, 10, 100, 100)}, Monitor{.rect = QRect(110, 10, 100, 100)}});
    QVERIFY(noPrimary[0].primary);
    QCOMPARE(noPrimary[0].rect.topLeft(), QPoint(0, 0));
    QVERIFY(!noPrimary[1].primary);
}

void MonitorLayoutTest::testLogicalPositions_data()
{
    QTest::addColumn<MonitorLayout>("layout");
    QTest::addColumn<QList<QPoint>>("positions");

    QTest::newRow("single") << MonitorLayout{Monitor{.rect = QRect(0, 0, 1920, 1080), .scale = 1.5, .primary = true}} << QList<QPoint>{QPoint(0, 0)};

    QTest::newRow("side by side") << MonitorLayout{
        Monitor{.rect = QRect(0, 0, 1920, 1080), .primary = true},
        Monitor{.rect = QRect(1920, 0, 1280, 1024)},
    } << QList<QPoint>{QPoint(0, 0), QPoint(1920, 0)};

    // A 2x monitor is half as wide logically, the next one must touch it.
    QTest::newRow("mixed scale") << MonitorLayout{
        Monitor{.rect = QRect(0, 0, 3840, 2160), .scale = 2.0, .primary = true},
        Monitor{.rect = QRect(3840, 0, 1920, 1080), .scale = 1.0},
    } << QList<QPoint>{QPoint(0, 0), QPoint(1920, 0)};

    // The upper monitor is 720 logical pixels high.
    QTest::newRow("stacked") << MonitorLayout{
        Monitor{.rect = QRect(0, 1080, 1920, 1080), .primary = true},
        Monitor{.rect = QRect(0, 0, 2880, 1080), .scale = 1.5},
    } << QList<QPoint>{QPoint(0, 720), QPoint(0, 0)};
}

void MonitorLayoutTest::testLogicalPositions()
{
    QFETCH(MonitorLayout, layout);
    QFETCH(QList<QPoint>, positions);
    QCOMPARE(logicalPositions(layout), positions);
}

void MonitorLayoutTest::testMonitorAt()
{
    const MonitorLayout layout{
        Monitor{.rect = QRect(0, 0, 1280, 1024), .primary = true},
        Monitor{.rect = QRect(1280, 0, 1024, 768)},
    };
    QCOMPARE(monitorAt(layout, QPointF(10, 10)), 0);
    QCOMPARE(monitorAt(layout, QPointF(1279.5, 500)), 0);
    QCOMPARE(monitorAt(layout, QPointF(1280, 500)), 1);
    QCOMPARE(monitorAt(layout, QPointF(2303, 767)), 1);
    // Below the second monitor, outside of all monitors.
    QCOMPARE(monitorAt(layout, QPointF(2000, 1000)), 1);
    QCOMPARE(monitorAt(layout, QPointF(-50, 10)), 0);
}

QTEST_GUILESS_MAIN(MonitorLayoutTest)

#include "monitorlayouttest.moc"
