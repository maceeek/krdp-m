// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include <cerrno>

#include <winpr/nt.h>

#include <DriveUtils.h>

using namespace KRdp;
using namespace Qt::StringLiterals;

class DriveUtilsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testValidFileName_data();
    void testValidFileName();
    void testSplitPath_data();
    void testSplitPath();
    void testParentAndFileName();
    void testAnnouncedDriveName();
    void testSanitizedShareName_data();
    void testSanitizedShareName();
    void testNtStatusToErrno_data();
    void testNtStatusToErrno();
    void testFileTime();
};

void DriveUtilsTest::testValidFileName_data()
{
    QTest::addColumn<QByteArray>("name");
    QTest::addColumn<bool>("valid");

    QTest::newRow("plain") << QByteArray("file.txt") << true;
    QTest::newRow("hidden") << QByteArray(".config") << true;
    QTest::newRow("spaces") << QByteArray("My Documents") << true;
    QTest::newRow("unicode") << QByteArray("Příliš žluťoučký kůň.txt") << true;
    QTest::newRow("emoji") << QByteArray("\xF0\x9F\x98\x80.png") << true;
    QTest::newRow("trailing dot") << QByteArray("file.") << true;
    QTest::newRow("three dots in name") << QByteArray("a...b") << true;
    // Not valid on Windows, but common on everything else. The client refuses them if it has to.
    QTest::newRow("colon") << QByteArray("Screenshot 12:30:01.png") << true;
    QTest::newRow("wildcards") << QByteArray("what?*.txt") << true;
    QTest::newRow("longest") << QByteArray(255, 'a') << true;

    QTest::newRow("empty") << QByteArray() << false;
    QTest::newRow("dot") << QByteArray(".") << false;
    QTest::newRow("dot dot") << QByteArray("..") << false;
    // Windows strips trailing dots and spaces, these end up as "..".
    QTest::newRow("dot dot space") << QByteArray(".. ") << false;
    QTest::newRow("three dots") << QByteArray("...") << false;
    QTest::newRow("dot space dot") << QByteArray(". .") << false;
    QTest::newRow("space") << QByteArray(" ") << false;
    QTest::newRow("slash") << QByteArray("a/b") << false;
    QTest::newRow("backslash") << QByteArray("a\\b") << false;
    QTest::newRow("backslash traversal") << QByteArray("..\\..\\secret") << false;
    QTest::newRow("only backslash") << QByteArray("\\") << false;
    QTest::newRow("embedded null") << QByteArray("a\0b", 3) << false;
    QTest::newRow("too long") << QByteArray(256, 'a') << false;
    QTest::newRow("invalid utf-8") << QByteArray("caf\xE9") << false;
    QTest::newRow("truncated utf-8") << QByteArray("a\xC3") << false;
    // An overlong encoding of '/', and one of '.'.
    QTest::newRow("overlong slash") << QByteArray(
        "a\xC0\xAF"
        "b") << false;
    QTest::newRow("overlong dots") << QByteArray("\xC0\xAE\xC0\xAE") << false;
    // A surrogate encoded as UTF-8 can not be converted to UTF-16 and back.
    QTest::newRow("encoded surrogate") << QByteArray("\xED\xA0\x80") << false;
}

void DriveUtilsTest::testValidFileName()
{
    QFETCH(QByteArray, name);
    QFETCH(bool, valid);
    QCOMPARE(DriveUtils::isValidFileName(name), valid);
}

void DriveUtilsTest::testSplitPath_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QByteArray>("share");
    QTest::addColumn<QByteArray>("sharePath");

    QTest::newRow("mount point") << QByteArray("/") << true << QByteArray() << QByteArray("\\");
    QTest::newRow("share") << QByteArray("/home") << true << QByteArray("home") << QByteArray("\\");
    QTest::newRow("file") << QByteArray("/home/file.txt") << true << QByteArray("home") << QByteArray("\\file.txt");
    QTest::newRow("nested") << QByteArray("/home/a b/c/d.txt") << true << QByteArray("home") << QByteArray("\\a b\\c\\d.txt");
    QTest::newRow("unicode") << QByteArray("/sdílení/složka/soubor") << true << QByteArray("sdílení") << QByteArray("\\složka\\soubor");

    QTest::newRow("empty") << QByteArray() << false << QByteArray() << QByteArray();
    QTest::newRow("relative") << QByteArray("home/file") << false << QByteArray() << QByteArray();
    QTest::newRow("dot dot") << QByteArray("/home/../etc/passwd") << false << QByteArray() << QByteArray();
    QTest::newRow("dot dot at end") << QByteArray("/home/..") << false << QByteArray() << QByteArray();
    QTest::newRow("dot dot share") << QByteArray("/../home") << false << QByteArray() << QByteArray();
    QTest::newRow("dot") << QByteArray("/home/./file") << false << QByteArray() << QByteArray();
    QTest::newRow("empty component") << QByteArray("/home//file") << false << QByteArray() << QByteArray();
    QTest::newRow("backslash traversal") << QByteArray("/home/..\\..\\Windows\\win.ini") << false << QByteArray() << QByteArray();
    QTest::newRow("backslash in share") << QByteArray("/ho\\me/file") << false << QByteArray() << QByteArray();
    QTest::newRow("dot dot space") << QByteArray("/home/.. /file") << false << QByteArray() << QByteArray();
    QTest::newRow("invalid utf-8") << QByteArray("/home/\xFF\xFE") << false << QByteArray() << QByteArray();
}

void DriveUtilsTest::testSplitPath()
{
    QFETCH(QByteArray, path);
    QFETCH(bool, valid);
    QFETCH(QByteArray, share);
    QFETCH(QByteArray, sharePath);

    const auto result = DriveUtils::splitPath(path);
    QCOMPARE(result.has_value(), valid);
    if (valid) {
        QCOMPARE(result->share, share);
        QCOMPARE(result->path, sharePath);
        // What is sent to the client never leaves the share.
        QVERIFY(!result->path.contains('/'));
        QVERIFY(!result->path.contains(".."));
    }
}

void DriveUtilsTest::testParentAndFileName()
{
    QCOMPARE(DriveUtils::parentPath("\\"), QByteArray("\\"));
    QCOMPARE(DriveUtils::parentPath("\\file"), QByteArray("\\"));
    QCOMPARE(DriveUtils::parentPath("\\dir\\file"), QByteArray("\\dir"));
    QCOMPARE(DriveUtils::parentPath("\\a\\b\\c"), QByteArray("\\a\\b"));

    QCOMPARE(DriveUtils::fileName("\\"), QByteArray());
    QCOMPARE(DriveUtils::fileName("\\file"), QByteArray("file"));
    QCOMPARE(DriveUtils::fileName("\\dir\\file.txt"), QByteArray("file.txt"));
}

void DriveUtilsTest::testAnnouncedDriveName()
{
    // FreeRDP sends the name as 8 bit characters.
    QCOMPARE(DriveUtils::announcedDriveName("test\0\0\0\0", QByteArrayView("test\0", 5)), u"test"_s);
    QCOMPARE(DriveUtils::announcedDriveName("document", QByteArrayView("documents\0", 10)), u"documents"_s);
    // Windows sends it as UTF-16.
    const char16_t wide[] = u"Žluťoučký on PC";
    QCOMPARE(DriveUtils::announcedDriveName("C\0\0\0\0\0\0\0", QByteArrayView(reinterpret_cast<const char *>(wide), sizeof(wide))), u"Žluťoučký on PC"_s);
    // Without device data there is only the DOS name, which is not necessarily terminated.
    QCOMPARE(DriveUtils::announcedDriveName(QByteArrayView("ABCDEFGH", 8), {}), u"ABCDEFGH"_s);
    QCOMPARE(DriveUtils::announcedDriveName(QByteArrayView("C:\0\0\0\0\0\0", 8), {}), u"C:"_s);
    QCOMPARE(DriveUtils::announcedDriveName(QByteArrayView("home\0\0\0\0", 8), QByteArrayView("\0", 1)), u"home"_s);
    QCOMPARE(DriveUtils::announcedDriveName({}, {}), QString());
}

void DriveUtilsTest::testSanitizedShareName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("expected");

    QTest::newRow("plain") << u"test"_s << QByteArray("test");
    QTest::newRow("unicode") << u"Sdílená složka"_s << QByteArray("Sdílená složka");
    QTest::newRow("windows drive") << u"C:"_s << QByteArray("C");
    QTest::newRow("surrounding spaces") << u"  home  "_s << QByteArray("home");
    QTest::newRow("slash") << u"a/b"_s << QByteArray("a_b");
    QTest::newRow("traversal") << u"../../etc"_s << QByteArray("drive7");
    QTest::newRow("backslash traversal") << u"..\\..\\etc"_s << QByteArray("drive7");
    QTest::newRow("dot dot") << u".."_s << QByteArray("drive7");
    QTest::newRow("dot") << u"."_s << QByteArray("drive7");
    QTest::newRow("hidden") << u".ssh"_s << QByteArray("drive7");
    QTest::newRow("empty") << QString() << QByteArray("drive7");
    QTest::newRow("only colon") << u":"_s << QByteArray("drive7");
    QTest::newRow("control characters") << u"a\nb\tc"_s << QByteArray("a_b_c");
    QTest::newRow("absolute") << u"/etc/passwd"_s << QByteArray("_etc_passwd");
    QTest::newRow("long") << QString(300, u'x') << QByteArray(64, 'x');
}

void DriveUtilsTest::testSanitizedShareName()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, expected);

    const auto result = DriveUtils::sanitizedShareName(name, 7);
    QCOMPARE(result, expected);
    QVERIFY(DriveUtils::isValidFileName(result));
}

void DriveUtilsTest::testNtStatusToErrno_data()
{
    QTest::addColumn<quint32>("status");
    QTest::addColumn<int>("error");

    QTest::newRow("success") << quint32(STATUS_SUCCESS) << 0;
    QTest::newRow("no such file") << quint32(STATUS_NO_SUCH_FILE) << ENOENT;
    QTest::newRow("name not found") << quint32(STATUS_OBJECT_NAME_NOT_FOUND) << ENOENT;
    QTest::newRow("path not found") << quint32(STATUS_OBJECT_PATH_NOT_FOUND) << ENOENT;
    QTest::newRow("name collision") << quint32(STATUS_OBJECT_NAME_COLLISION) << EEXIST;
    QTest::newRow("access denied") << quint32(STATUS_ACCESS_DENIED) << EACCES;
    QTest::newRow("write protected") << quint32(STATUS_MEDIA_WRITE_PROTECTED) << EROFS;
    QTest::newRow("sharing violation") << quint32(STATUS_SHARING_VIOLATION) << EBUSY;
    QTest::newRow("disk full") << quint32(STATUS_DISK_FULL) << ENOSPC;
    QTest::newRow("directory not empty") << quint32(STATUS_DIRECTORY_NOT_EMPTY) << ENOTEMPTY;
    QTest::newRow("not a directory") << quint32(STATUS_NOT_A_DIRECTORY) << ENOTDIR;
    QTest::newRow("is a directory") << quint32(STATUS_FILE_IS_A_DIRECTORY) << EISDIR;
    QTest::newRow("invalid name") << quint32(STATUS_OBJECT_NAME_INVALID) << EINVAL;
    QTest::newRow("invalid parameter") << quint32(STATUS_INVALID_PARAMETER) << EINVAL;
    QTest::newRow("invalid handle") << quint32(STATUS_INVALID_HANDLE) << EBADF;
    QTest::newRow("not supported") << quint32(STATUS_NOT_SUPPORTED) << ENOTSUP;
    QTest::newRow("not same device") << quint32(STATUS_NOT_SAME_DEVICE) << EXDEV;
    QTest::newRow("no memory") << quint32(STATUS_NO_MEMORY) << ENOMEM;
    QTest::newRow("timeout") << quint32(STATUS_IO_TIMEOUT) << ETIMEDOUT;
    QTest::newRow("unsuccessful") << quint32(STATUS_UNSUCCESSFUL) << EIO;
    // Anything unknown is an error, never success.
    QTest::newRow("unknown error") << quint32(0xC0FFEE00) << EIO;
    QTest::newRow("unknown warning") << quint32(0x80001234) << EIO;
    QTest::newRow("unknown success") << quint32(0x00000123) << EIO;
    QTest::newRow("win32 error") << quint32(87) << EIO;
}

void DriveUtilsTest::testNtStatusToErrno()
{
    QFETCH(quint32, status);
    QFETCH(int, error);
    QCOMPARE(DriveUtils::ntStatusToErrno(status), error);
}

void DriveUtilsTest::testFileTime()
{
    // 2000-01-01 00:00:00 UTC
    const qint64 fileTime = 125911584000000000LL;
    const auto time = DriveUtils::fileTimeToTimespec(fileTime);
    QCOMPARE(time.tv_sec, 946684800);
    QCOMPARE(time.tv_nsec, 0);
    QCOMPARE(DriveUtils::timespecToFileTime(time), fileTime);

    const auto precise = DriveUtils::fileTimeToTimespec(fileTime + 1234567);
    QCOMPARE(precise.tv_sec, 946684800);
    QCOMPARE(precise.tv_nsec, 123456700);
    QCOMPARE(DriveUtils::timespecToFileTime(precise), fileTime + 1234567);

    // An unset time, and one before 1970.
    QCOMPARE(DriveUtils::fileTimeToTimespec(0).tv_sec, 0);
    QCOMPARE(DriveUtils::fileTimeToTimespec(-5).tv_sec, 0);
    QCOMPARE(DriveUtils::fileTimeToTimespec(1000).tv_sec, 0);

    QCOMPARE(DriveUtils::timespecToFileTime(timespec{.tv_sec = 0, .tv_nsec = 0}), 116444736000000000LL);
}

QTEST_GUILESS_MAIN(DriveUtilsTest)

#include "driveutilstest.moc"
