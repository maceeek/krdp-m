// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QSignalSpy>
#include <QTest>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "NvencEncoder.h"

using namespace KRdp;

/**
 * Without an NVIDIA GPU, NvencEncoder is tested with libx264. Everything but
 * the encoder options is the same.
 */
class NvencEncoderTest : public QObject
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
     * Decode \p packets, as a client would, returning the last frame in RGB.
     */
    static QImage decode(const QList<Packet> &packets, bool *fullRange = nullptr)
    {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        AVCodecContext *context = avcodec_alloc_context3(codec);
        avcodec_open2(context, codec, nullptr);
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        QImage result;
        for (const auto &data : packets) {
            av_new_packet(packet, data.data.size());
            memcpy(packet->data, data.data.constData(), data.data.size());
            avcodec_send_packet(context, packet);
            av_packet_unref(packet);
            while (avcodec_receive_frame(context, frame) == 0) {
                if (fullRange) {
                    *fullRange = frame->color_range == AVCOL_RANGE_JPEG;
                }
                result = QImage(frame->width, frame->height, QImage::Format_RGB32);
                // Clients convert with full range BT.601.
                SwsContext *sws = sws_getContext(frame->width,
                                                 frame->height,
                                                 AVPixelFormat(frame->format),
                                                 frame->width,
                                                 frame->height,
                                                 AV_PIX_FMT_BGRA,
                                                 SWS_POINT,
                                                 nullptr,
                                                 nullptr,
                                                 nullptr);
                const int *coefficients = sws_getCoefficients(SWS_CS_ITU601);
                sws_setColorspaceDetails(sws, coefficients, 1, coefficients, 1, 0, 1 << 16, 1 << 16);
                uint8_t *destination[] = {result.bits()};
                const int stride[] = {int(result.bytesPerLine())};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, destination, stride);
                sws_freeContext(sws);
            }
        }

        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
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
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("FFmpeg has no libx264");
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

        NvencEncoder encoder(QByteArrayLiteral("libx264"));
        encoder.setQuality(100);
        QSignalSpy packets(&encoder, &NvencEncoder::packetReady);

        encoder.encode(solidImage(QSize(64, 48), color, format));
        const Packet packet = takePacket(packets);
        QVERIFY(!packet.data.isEmpty());
        QVERIFY(packet.isKeyFrame);
        QCOMPARE(packet.size, QSize(64, 48));

        bool fullRange = false;
        const QImage decoded = decode({packet}, &fullRange);
        QCOMPARE(decoded.size(), QSize(64, 48));
        QVERIFY(fullRange);
        compareColor(decoded.pixelColor(32, 24), color);
    }

    void testKeyFrameRequest()
    {
        NvencEncoder encoder(QByteArrayLiteral("libx264"));
        QSignalSpy packets(&encoder, &NvencEncoder::packetReady);

        encoder.encode(solidImage(QSize(64, 48), Qt::red));
        const Packet first = takePacket(packets);
        QVERIFY(first.isKeyFrame);

        encoder.encode(solidImage(QSize(64, 48), Qt::green));
        const Packet second = takePacket(packets);
        QVERIFY(!second.data.isEmpty());
        QVERIFY(!second.isKeyFrame);

        // Without a new frame the last one is encoded again.
        encoder.requestKeyFrame();
        const Packet third = takePacket(packets);
        QVERIFY(third.isKeyFrame);
        compareColor(decode({third}).pixelColor(10, 10), QColor(0, 255, 0));
    }

    void testSizeChange()
    {
        NvencEncoder encoder(QByteArrayLiteral("libx264"));
        QSignalSpy packets(&encoder, &NvencEncoder::packetReady);

        encoder.encode(solidImage(QSize(64, 48), Qt::red));
        QCOMPARE(takePacket(packets).size, QSize(64, 48));

        // Large enough to be converted in several slices.
        QImage image = solidImage(QSize(1920, 1080), Qt::blue);
        image.setPixelColor(1900, 1060, Qt::white);
        encoder.encode(image);
        const Packet resized = takePacket(packets);
        QCOMPARE(resized.size, QSize(1920, 1080));
        QVERIFY(resized.isKeyFrame);
        const QImage decoded = decode({resized});
        QCOMPARE(decoded.size(), QSize(1920, 1080));
        compareColor(decoded.pixelColor(10, 10), QColor(0, 0, 255));
        compareColor(decoded.pixelColor(1000, 600), QColor(0, 0, 255));
        compareColor(decoded.pixelColor(1910, 1070), QColor(0, 0, 255));
    }

    void testOddSize()
    {
        NvencEncoder encoder(QByteArrayLiteral("libx264"));
        QSignalSpy packets(&encoder, &NvencEncoder::packetReady);

        encoder.encode(solidImage(QSize(63, 47), Qt::white));
        const Packet packet = takePacket(packets);
        QCOMPARE(packet.size, QSize(63, 47));

        // Padded with black to an even size.
        const QImage decoded = decode({packet});
        QCOMPARE(decoded.size(), QSize(64, 48));
        compareColor(decoded.pixelColor(62, 46), QColor(Qt::white));
        compareColor(decoded.pixelColor(63, 10), QColor(Qt::black));
        compareColor(decoded.pixelColor(10, 47), QColor(Qt::black));
    }

    void testLatestFrameWins()
    {
        NvencEncoder encoder(QByteArrayLiteral("libx264"));
        QSignalSpy packets(&encoder, &NvencEncoder::packetReady);

        for (int i = 0; i < 50; ++i) {
            encoder.encode(solidImage(QSize(64, 48), QColor(i * 5, 0, 0)));
        }
        QList<Packet> received;
        while (true) {
            Packet packet = takePacket(packets);
            if (packet.data.isEmpty()) {
                break;
            }
            received.append(packet);
            if (packets.isEmpty() && !packets.wait(500)) {
                break;
            }
        }
        QVERIFY(!received.isEmpty());
        QVERIFY(received.size() <= 50);
        compareColor(decode(received).pixelColor(10, 10), QColor(245, 0, 0));
    }

    void testMissingEncoder()
    {
        NvencEncoder encoder(QByteArrayLiteral("does-not-exist"));
        QSignalSpy failed(&encoder, &NvencEncoder::failed);
        encoder.encode(solidImage(QSize(64, 48), Qt::red));
        QVERIFY(failed.wait(5000));
    }
};

QTEST_GUILESS_MAIN(NvencEncoderTest)

#include "nvencencodertest.moc"
