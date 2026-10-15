// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QTest>

#include "NetworkDetection_p.h"

using namespace KRdp;
using Clock = std::chrono::system_clock;

class NetworkDetectionSequenceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void responsesAfterMultipleWraps()
    {
        uint16_t counter = 0;
        QHash<uint16_t, Clock::time_point> requests;
        const auto sent = Clock::now();

        // More than three full wire-sequence cycles, with network result
        // PDUs sharing the counter as they do in NetworkDetection.
        for (int request = 0; request < 200000; ++request) {
            const auto sequence = nextRdpSequenceNumber(counter, requests);
            QVERIFY(sequence);
            QVERIFY(*sequence != 0);
            requests.insert(*sequence, sent);

            const uint16_t reply = *sequence;
            QVERIFY(requests.contains(reply));
            QCOMPARE(requests.take(reply), sent);

            if (request % 14 == 0) {
                QVERIFY(nextRdpSequenceNumber(counter, requests));
            }
        }
        QVERIFY(requests.isEmpty());
    }

    void outstandingRequestsSurviveWrap()
    {
        uint16_t counter = 65534;
        QHash<uint16_t, Clock::time_point> requests;
        const auto sent = Clock::now();
        requests.insert(65535, sent);
        requests.insert(1, sent);
        requests.insert(2, sent);

        const auto sequence = nextRdpSequenceNumber(counter, requests);
        QVERIFY(sequence);
        QCOMPARE(*sequence, uint16_t(3));
        QCOMPARE(requests.size(), 3);
        QCOMPARE(requests.value(1), sent);
    }

    void exhaustedSequenceNumbers()
    {
        uint16_t counter = 65534;
        QHash<uint16_t, Clock::time_point> requests;
        const auto sent = Clock::now();
        for (uint32_t sequence = 1; sequence <= 65535; ++sequence) {
            requests.insert(uint16_t(sequence), sent);
        }

        QVERIFY(!nextRdpSequenceNumber(counter, requests));
        QCOMPARE(requests.size(), 65535);

        // A late response frees a number and measurements can resume.
        requests.remove(42);
        const auto sequence = nextRdpSequenceNumber(counter, requests);
        QVERIFY(sequence);
        QCOMPARE(*sequence, uint16_t(42));
    }
};

QTEST_GUILESS_MAIN(NetworkDetectionSequenceTest)

#include "networkdetectionsequencetest.moc"
