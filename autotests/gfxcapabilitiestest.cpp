// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "GfxCapabilities.h"

using namespace KRdp;

Q_DECLARE_METATYPE(KRdp::GfxCodec)

/**
 * AV1 is only chosen for clients advertising FreeRDP's extension, every
 * other client is treated as before.
 */
class GfxCapabilitiesTest : public QObject
{
    Q_OBJECT

private:
    static RDPGFX_CAPSET capsSet(uint32_t version, uint32_t flags = 0)
    {
        RDPGFX_CAPSET set = {};
        set.version = version;
        set.length = 4;
        set.flags = flags;
        return set;
    }

    // What the Windows Remote Desktop client advertises.
    static QList<RDPGFX_CAPSET> windowsCaps(uint32_t flags = 0)
    {
        return {capsSet(RDPGFX_CAPVERSION_107, flags),
                capsSet(RDPGFX_CAPVERSION_106, flags),
                capsSet(RDPGFX_CAPVERSION_105, flags),
                capsSet(RDPGFX_CAPVERSION_104, flags),
                capsSet(RDPGFX_CAPVERSION_103, flags),
                capsSet(RDPGFX_CAPVERSION_102, flags),
                capsSet(RDPGFX_CAPVERSION_101, flags),
                capsSet(RDPGFX_CAPVERSION_10, flags),
                capsSet(RDPGFX_CAPVERSION_81, RDPGFX_CAPS_FLAG_AVC420_ENABLED),
                capsSet(RDPGFX_CAPVERSION_8)};
    }

    // What a FreeRDP client built with AV1 advertises, the AV1 extension first.
    static QList<RDPGFX_CAPSET> freeRdpAv1Caps(uint32_t flags = 0)
    {
        auto caps = windowsCaps(flags);
        if (flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
            caps[8].flags = 0;
        }
        caps.prepend(capsSet(uint32_t(RDPGFX_CAPVERSION_FRDP_1), flags | RDPGFX_CAPS_FLAG_AV1_I444_SUPPORTED));
        return caps;
    }

private Q_SLOTS:
    void testChoose_data()
    {
        QTest::addColumn<QList<RDPGFX_CAPSET>>("caps");
        QTest::addColumn<bool>("h264");
        QTest::addColumn<bool>("av1");
        QTest::addColumn<GfxCodec>("codec");
        QTest::addColumn<uint32_t>("version");
        QTest::addColumn<bool>("h264Supported");

        QTest::newRow("Windows client") << windowsCaps() << true << true << GfxCodec::H264 << uint32_t(RDPGFX_CAPVERSION_107) << true;
        QTest::newRow("Windows client, H.264 disabled") << windowsCaps() << false << true << GfxCodec::Progressive << uint32_t(RDPGFX_CAPVERSION_107) << false;
        QTest::newRow("FreeRDP-M client") << freeRdpAv1Caps() << true << true << GfxCodec::Av1 << uint32_t(RDPGFX_CAPVERSION_FRDP_1) << true;
        QTest::newRow("FreeRDP-M client, no AV1 encoder") << freeRdpAv1Caps() << true << false << GfxCodec::H264 << uint32_t(RDPGFX_CAPVERSION_107) << true;
        QTest::newRow("FreeRDP-M client without H.264")
            << freeRdpAv1Caps(RDPGFX_CAPS_FLAG_AVC_DISABLED) << true << true << GfxCodec::Av1 << uint32_t(RDPGFX_CAPVERSION_FRDP_1) << false;
        QTest::newRow("FreeRDP-M client without H.264, no AV1 encoder")
            << freeRdpAv1Caps(RDPGFX_CAPS_FLAG_AVC_DISABLED) << true << false << GfxCodec::Progressive << uint32_t(RDPGFX_CAPVERSION_107) << false;
        QTest::newRow("FreeRDP-M client, H.264 disabled") << freeRdpAv1Caps() << false << true << GfxCodec::Av1 << uint32_t(RDPGFX_CAPVERSION_FRDP_1) << false;
        QTest::newRow("RDP 8") << QList<RDPGFX_CAPSET>{capsSet(RDPGFX_CAPVERSION_8)} << true << true << GfxCodec::Progressive << uint32_t(RDPGFX_CAPVERSION_8)
                               << false;
        QTest::newRow("RDP 8.1 with AVC420") << QList<RDPGFX_CAPSET>{capsSet(RDPGFX_CAPVERSION_8),
                                                                     capsSet(RDPGFX_CAPVERSION_81, RDPGFX_CAPS_FLAG_AVC420_ENABLED)}
                                             << true << true << GfxCodec::H264 << uint32_t(RDPGFX_CAPVERSION_81) << true;
        QTest::newRow("only the AV1 extension") << QList<RDPGFX_CAPSET>{capsSet(uint32_t(RDPGFX_CAPVERSION_FRDP_1))} << true << false << GfxCodec::Progressive
                                                << uint32_t(RDPGFX_CAPVERSION_FRDP_1) << false;
    }

    void testChoose()
    {
        QFETCH(QList<RDPGFX_CAPSET>, caps);
        QFETCH(bool, h264);
        QFETCH(bool, av1);

        const auto capabilities = chooseGfxCapabilities(caps.constData(), caps.size(), h264, av1);
        QVERIFY(capabilities);
        QTEST(capabilities->codec, "codec");
        QTEST(capabilities->capsSet.version, "version");
        QTEST(capabilities->h264Supported, "h264Supported");

        // The flags are confirmed as the client advertised them.
        const auto advertised = std::find_if(caps.cbegin(), caps.cend(), [&capabilities](const RDPGFX_CAPSET &set) {
            return set.version == capabilities->capsSet.version;
        });
        QCOMPARE(capabilities->capsSet.flags, advertised->flags);
    }

    void testNoCaps()
    {
        QVERIFY(!chooseGfxCapabilities(nullptr, 0, true, true));
    }
};

QTEST_GUILESS_MAIN(GfxCapabilitiesTest)

#include "gfxcapabilitiestest.moc"
