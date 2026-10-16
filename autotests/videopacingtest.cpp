// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VideoStream_p.h"
#include <QTest>

using namespace KRdp;
using namespace std::chrono_literals;

class VideoPacingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void newSurfaceWaitsForKeyFrame()
    {
        VideoFrameGate gate;
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::RequestKeyFrame);
        // More packets may already be in flight before the request is handled.
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::Wait);
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::Wait);
        QCOMPARE(gate.submit(true), VideoFrameGate::Action::Send);
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::Send);
        // Recreating the surface invalidates its references again.
        gate = {};
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::RequestKeyFrame);
        QCOMPARE(gate.submit(true), VideoFrameGate::Action::Send);
    }

    void firstKeyFrameNeedsNoRequest()
    {
        VideoFrameGate gate;
        QCOMPARE(gate.submit(true), VideoFrameGate::Action::Send);
        QCOMPARE(gate.submit(false), VideoFrameGate::Action::Send);
    }

    void wifiJitterKeepsSixtyHz()
    {
        for (auto turnaround : {1ms, 10ms, 20ms, 40ms, 80ms}) {
            const auto pacing = videoPacing(turnaround);
            QCOMPARE(pacing.frameRate, 60);
            QVERIFY(pacing.inFlightLimit >= 2);
            QVERIFY(pacing.inFlightLimit <= 6);
        }
    }

    void slowClientIsBounded()
    {
        const auto slow = videoPacing(200ms);
        QCOMPARE(slow.inFlightLimit, 6);
        QVERIFY(slow.frameRate < 30);
        QVERIFY(slow.frameRate >= 1);
        QCOMPARE(videoPacing(10000ms).frameRate, 1);
        QCOMPARE(videoPacing(0ms).frameRate, 60);
    }
};
QTEST_GUILESS_MAIN(VideoPacingTest)
#include "videopacingtest.moc"
