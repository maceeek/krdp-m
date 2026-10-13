// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QSignalSpy>
#include <QTest>

#include <freerdp/codec/av1.h>
#include <freerdp/codec/color.h>

#include "Av1Encoder.h"

using namespace KRdp;

/**
 * Needs a GPU that encodes AV1, decodes the packets like a FreeRDP-M client.
 */
class Av1EncoderTest : public QObject
{
    Q_OBJECT

private:
    struct Packet {
        QByteArray data;
        bool isKeyFrame;
        QSize size;
    };

    static Packet takePacket(QSignalSpy &spy)
    {
        if (spy.isEmpty()) {
            spy.wait(5000);
        }
        if (spy.isEmpty()) {
            return {};
        }
        const auto arguments = spy.takeFirst();
        return {arguments.at(0).toByteArray(), arguments.at(1).toBool(), arguments.at(2).toSize()};
    }

    static QImage solidImage(const QSize &size, const QColor &color, QImage::Format format = QImage::Format_RGB32)
    {
        QImage image(size, format);
        image.fill(color);
        return image;
    }

    /**
     * Decode \p packets with a new decoder, returning the last frame.
     */
    static QImage decode(const QList<Packet> &packets)
    {
        if (packets.isEmpty()) {
            return QImage();
        }
        const QSize size = packets.last().size;
        FREERDP_AV1_CONTEXT *decoder = freerdp_av1_context_new(FALSE);
        if (!decoder || !freerdp_av1_context_reset(decoder, size.width(), size.height())) {
            freerdp_av1_context_free(decoder);
            return QImage();
        }

        QImage result(size, QImage::Format_RGB32);
        result.fill(Qt::magenta);
        const RECTANGLE_16 rect = {0, 0, UINT16(size.width()), UINT16(size.height())};
        for (const auto &packet : packets) {
            const INT32 status = freerdp_av1_decompress(decoder,
                                                        reinterpret_cast<const BYTE *>(packet.data.constData()),
                                                        packet.data.size(),
                                                        result.bits(),
                                                        PIXEL_FORMAT_BGRX32,
                                                        result.bytesPerLine(),
                                                        size.width(),
                                                        size.height(),
                                                        &rect,
                                                        1);
            if (status < 0) {
                result = QImage();
                break;
            }
        }
        freerdp_av1_context_free(decoder);
        return result;
    }

    static void compareColor(const QColor &actual, const QColor &expected)
    {
        constexpr int tolerance = 6;
        QVERIFY2(std::abs(actual.red() - expected.red()) <= tolerance && std::abs(actual.green() - expected.green()) <= tolerance
                     && std::abs(actual.blue() - expected.blue()) <= tolerance,
                 qPrintable(QStringLiteral("%1 is not %2").arg(actual.name(), expected.name())));
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!Av1Encoder::isAvailable()) {
            QSKIP("No GPU encodes AV1");
        }
    }

    void testColors_data()
    {
        QTest::addColumn<QColor>("color");
        QTest::addColumn<QImage::Format>("format");

        QTest::newRow("white") << QColor(Qt::white) << QImage::Format_RGB32;
        QTest::newRow("black") << QColor(Qt::black) << QImage::Format_RGB32;
        QTest::newRow("red") << QColor(255, 0, 0) << QImage::Format_RGB32;
        QTest::newRow("blue, from a DMA-BUF") << QColor(0, 0, 255) << QImage::Format_RGBA8888_Premultiplied;
        QTest::newRow("gray, other format") << QColor(128, 128, 128) << QImage::Format_RGB888;
    }

    void testColors()
    {
        QFETCH(QColor, color);
        QFETCH(QImage::Format, format);

        Av1Encoder encoder;
        encoder.setQuality(100);
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        encoder.encode(solidImage(QSize(320, 240), color, format));
        const Packet packet = takePacket(packets);
        QVERIFY(!packet.data.isEmpty());
        QVERIFY(packet.isKeyFrame);
        QCOMPARE(packet.size, QSize(320, 240));

        const QImage decoded = decode({packet});
        QCOMPARE(decoded.size(), QSize(320, 240));
        compareColor(decoded.pixelColor(160, 120), color);
    }

    void testKeyFrameRequest()
    {
        Av1Encoder encoder;
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        encoder.encode(solidImage(QSize(320, 240), Qt::red));
        const Packet first = takePacket(packets);
        QVERIFY(first.isKeyFrame);

        encoder.encode(solidImage(QSize(320, 240), Qt::green));
        const Packet second = takePacket(packets);
        QVERIFY(!second.data.isEmpty());
        QVERIFY(!second.isKeyFrame);
        compareColor(decode({first, second}).pixelColor(10, 10), QColor(0, 255, 0));

        // Without a new frame the last one is encoded again, decodable on its own.
        encoder.requestKeyFrame();
        const Packet third = takePacket(packets);
        QVERIFY(third.isKeyFrame);
        compareColor(decode({third}).pixelColor(10, 10), QColor(0, 255, 0));
    }

    void testSizeChange()
    {
        Av1Encoder encoder;
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        encoder.encode(solidImage(QSize(320, 240), Qt::red));
        QCOMPARE(takePacket(packets).size, QSize(320, 240));

        QImage image = solidImage(QSize(1920, 1080), Qt::blue);
        encoder.encode(image);
        const Packet resized = takePacket(packets);
        QCOMPARE(resized.size, QSize(1920, 1080));
        QVERIFY(resized.isKeyFrame);
        const QImage decoded = decode({resized});
        QCOMPARE(decoded.size(), QSize(1920, 1080));
        compareColor(decoded.pixelColor(10, 10), QColor(0, 0, 255));
        compareColor(decoded.pixelColor(1910, 1070), QColor(0, 0, 255));
    }

    void testOddSize()
    {
        Av1Encoder encoder;
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        // The last column and row are not cut off. Colors are shared by 2x2
        // pixels, a stripe of a single pixel would not keep its color.
        QImage image = solidImage(QSize(1279, 719), Qt::white);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = image.width() - 16; x < image.width(); ++x) {
                image.setPixelColor(x, y, Qt::red);
            }
        }
        encoder.encode(image);
        const Packet packet = takePacket(packets);
        QCOMPARE(packet.size, QSize(1279, 719));

        const QImage decoded = decode({packet});
        QCOMPARE(decoded.size(), QSize(1279, 719));
        compareColor(decoded.pixelColor(640, 360), QColor(Qt::white));
        compareColor(decoded.pixelColor(1278, 718), QColor(Qt::red));
    }

    void testQualityChange()
    {
        Av1Encoder encoder;
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        encoder.encode(solidImage(QSize(320, 240), Qt::red));
        QVERIFY(takePacket(packets).isKeyFrame);

        encoder.setQuality(10);
        encoder.encode(solidImage(QSize(320, 240), Qt::blue));
        const Packet packet = takePacket(packets);
        QVERIFY(packet.isKeyFrame);
        compareColor(decode({packet}).pixelColor(10, 10), QColor(0, 0, 255));
    }

    void testBackpressure()
    {
        Av1Encoder encoder;
        QSignalSpy packets(&encoder, &Av1Encoder::packetReady);

        encoder.encode(solidImage(QSize(320, 240), Qt::red));
        // The packet may arrive before waiting would start.
        QTRY_COMPARE_WITH_TIMEOUT(packets.count(), 1, 5000);

        // Nothing is encoded while the packet was not sent.
        encoder.encode(solidImage(QSize(320, 240), Qt::green));
        encoder.encode(solidImage(QSize(320, 240), Qt::blue));
        QVERIFY(!packets.wait(300));
        QCOMPARE(packets.count(), 1);

        // Then only the latest frame is.
        const Packet first = takePacket(packets);
        const Packet second = takePacket(packets);
        QVERIFY(!second.data.isEmpty());
        compareColor(decode({first, second}).pixelColor(10, 10), QColor(0, 0, 255));
        QVERIFY(!packets.wait(300));
    }
};

QTEST_GUILESS_MAIN(Av1EncoderTest)

#include "av1encodertest.moc"
