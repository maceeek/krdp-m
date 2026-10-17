// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include <VideoStream.h>

namespace
{
QList<QSize> sourceRequests;
QList<QSize> encodedRequests;
}

// Interpose these entry points at the executable boundary. The real
// VideoStream and KPipeWire stream objects still run, but no PipeWire server
// or GPU is needed to observe which sizes VideoStream asks them to negotiate.
void PipeWireSourceStream::setRequestedSize(const QSize &size)
{
    sourceRequests.append(size);
}

void PipeWireBaseEncodedStream::setRequestedSize(const QSize &size)
{
    encodedRequests.append(size);
}

namespace KRdp
{
class VideoStreamSizeTest : public QObject
{
    Q_OBJECT

    static MonitorLayout layout(const QSize &size)
    {
        return {Monitor{.rect = QRect(QPoint(), size), .primary = true}};
    }

private Q_SLOTS:
    void initTestCase()
    {
        // Exercise KPipeWire's encoded H.264 path on machines with NVENC too.
        qputenv("KRDP_DISABLE_NVENC", "1");
    }

    void init()
    {
        sourceRequests.clear();
        encodedRequests.clear();
    }

    void resize_data()
    {
        QTest::addColumn<VideoStream::EncodingMode>("mode");
        QTest::newRow("progressive") << VideoStream::EncodingMode::Progressive;
        QTest::newRow("av1-source") << VideoStream::EncodingMode::AV1;
        QTest::newRow("h264-encoded") << VideoStream::EncodingMode::H264;
    }

    void resize()
    {
        QFETCH(VideoStream::EncodingMode, mode);
        auto &requests = mode == VideoStream::EncodingMode::H264 ? encodedRequests : sourceRequests;
        VideoStream stream(nullptr);

        // A layout can arrive before codec negotiation creates the stream.
        stream.setMonitorLayout(layout(QSize(1488, 1000)));
        stream.setActiveEncodingMode(mode);
        QCOMPARE(requests, QList<QSize>{QSize(1488, 1000)});

        // Resizing the output alone leaves the consumer at its old size.
        // Fullscreen must also request a new PipeWire format.
        requests.clear();
        stream.setMonitorLayout(layout(QSize(1920, 1200)));
        QCOMPARE(requests, QList<QSize>{QSize(1920, 1200)});

        // Encoder recreation must retain the negotiated target size.
        requests.clear();
        stream.createStream(0);
        QCOMPARE(requests, QList<QSize>{QSize(1920, 1200)});

        requests.clear();
        stream.setMonitorLayout(layout(QSize(1488, 1000)));
        QCOMPARE(requests, QList<QSize>{QSize(1488, 1000)});
    }

    void multipleMonitors()
    {
        VideoStream stream(nullptr);
        MonitorLayout monitors{Monitor{.rect = QRect(0, 0, 1488, 1000), .primary = true}, Monitor{.rect = QRect(1488, 0, 1280, 720)}};
        stream.setMonitorLayout(monitors);
        stream.setActiveEncodingMode(VideoStream::EncodingMode::Progressive);
        QCOMPARE(sourceRequests, (QList<QSize>{QSize(1488, 1000), QSize(1280, 720)}));

        sourceRequests.clear();
        monitors[1].rect.setSize(QSize(1920, 1200));
        stream.setMonitorLayout(monitors);
        QCOMPARE(sourceRequests, QList<QSize>{QSize(1920, 1200)});
    }

    void codecFallbackKeepsSize()
    {
        VideoStream stream(nullptr);
        stream.setMonitorLayout(layout(QSize(1920, 1200)));
        stream.setActiveEncodingMode(VideoStream::EncodingMode::AV1);
        QCOMPARE(sourceRequests, QList<QSize>{QSize(1920, 1200)});
        stream.setActiveEncodingMode(VideoStream::EncodingMode::H264);
        QCOMPARE(encodedRequests, QList<QSize>{QSize(1920, 1200)});
    }
};
}

QTEST_GUILESS_MAIN(KRdp::VideoStreamSizeTest)
#include "videostreamsizetest.moc"
