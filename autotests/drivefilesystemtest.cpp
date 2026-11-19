// SPDX-FileCopyrightText: 2026 KRdp-M contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <DriveFilesystem.h>

using namespace KRdp;
using namespace Qt::StringLiterals;

/**
 * A drive that keeps its files in a local directory, standing in for a client.
 *
 * It remembers every path it is asked for, so that the tests can verify that
 * the file system never asks for anything outside of the drive, and it can be
 * told to misbehave like a hostile client would.
 */
class LocalBackend : public DriveBackend
{
public:
    explicit LocalBackend(const QString &root)
        : m_root(QFile::encodeName(root))
    {
    }

    ~LocalBackend() override
    {
        for (const int descriptor : std::as_const(m_files)) {
            ::close(descriptor);
        }
    }

    // Entries added to every listing.
    QList<DriveEntry> extraEntries;
    // Makes every request fail, like after the connection was lost.
    std::atomic<bool> broken = false;
    std::atomic<int> removeDirectoryCalls = 0;

    QList<QByteArray> seenPaths()
    {
        std::lock_guard lock(m_mutex);
        return m_seenPaths;
    }

    int openFileCount()
    {
        std::lock_guard lock(m_mutex);
        return m_files.size();
    }

    int list(const QByteArray &path, QList<DriveEntry> &entries) override
    {
        const QByteArray directory = local(path);
        if (broken) {
            return -EIO;
        }
        DIR *handle = ::opendir(directory.constData());
        if (!handle) {
            return -errno;
        }
        // Like a client, including "." and "..".
        while (const dirent *item = ::readdir(handle)) {
            struct stat st;
            if (::lstat(QByteArray(directory + '/' + item->d_name).constData(), &st) != 0) {
                continue;
            }
            DriveEntry entry;
            entry.name = item->d_name;
            entry.directory = S_ISDIR(st.st_mode);
            entry.readOnly = !(st.st_mode & S_IWUSR);
            entry.size = quint64(st.st_size);
            entry.accessTime = st.st_atim;
            entry.modificationTime = st.st_mtim;
            entry.changeTime = st.st_ctim;
            entries.append(entry);
        }
        ::closedir(handle);
        entries.append(extraEntries);
        return 0;
    }

    int open(const QByteArray &path, Access access, Disposition disposition, quint32 &fileId) override
    {
        const QByteArray file = local(path);
        if (broken) {
            return -EIO;
        }
        int flags = access == Access::Read ? O_RDONLY : (access == Access::Write ? O_WRONLY : O_RDWR);
        switch (disposition) {
        case Disposition::Open:
            break;
        case Disposition::Truncate:
            flags |= O_TRUNC;
            break;
        case Disposition::Create:
            flags |= O_CREAT | O_EXCL;
            break;
        case Disposition::OpenOrCreate:
            flags |= O_CREAT;
            break;
        case Disposition::CreateOrTruncate:
            flags |= O_CREAT | O_TRUNC;
            break;
        }
        const int descriptor = ::open(file.constData(), flags | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor < 0) {
            return -errno;
        }
        std::lock_guard lock(m_mutex);
        fileId = m_nextFileId++;
        m_files.insert(fileId, descriptor);
        return 0;
    }

    int close(quint32 fileId) override
    {
        std::lock_guard lock(m_mutex);
        const int descriptor = m_files.take(fileId);
        return descriptor > 0 && ::close(descriptor) == 0 ? 0 : -EBADF;
    }

    int read(quint32 fileId, char *data, quint32 size, quint64 offset) override
    {
        const int descriptor = this->descriptor(fileId);
        if (broken || descriptor < 0) {
            return broken ? -EIO : -EBADF;
        }
        const auto result = ::pread(descriptor, data, size, off_t(offset));
        return result < 0 ? -errno : int(result);
    }

    int write(quint32 fileId, const char *data, quint32 size, quint64 offset) override
    {
        const int descriptor = this->descriptor(fileId);
        if (broken || descriptor < 0) {
            return broken ? -EIO : -EBADF;
        }
        const auto result = ::pwrite(descriptor, data, size, off_t(offset));
        return result < 0 ? -errno : int(result);
    }

    int createDirectory(const QByteArray &path) override
    {
        return ::mkdir(local(path).constData(), 0755) == 0 ? 0 : -errno;
    }

    int removeDirectory(const QByteArray &path) override
    {
        ++removeDirectoryCalls;
        return ::rmdir(local(path).constData()) == 0 ? 0 : -errno;
    }

    int removeFile(const QByteArray &path) override
    {
        return ::unlink(local(path).constData()) == 0 ? 0 : -errno;
    }

    int rename(const QByteArray &from, const QByteArray &to, bool replace) override
    {
        const QByteArray source = local(from);
        const QByteArray target = local(to);
        return ::renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), replace ? 0 : RENAME_NOREPLACE) == 0 ? 0 : -errno;
    }

    int resize(const QByteArray &path, quint64 size) override
    {
        return ::truncate(local(path).constData(), off_t(size)) == 0 ? 0 : -errno;
    }

    int setTimes(const QByteArray &path, const timespec *accessTime, const timespec *modificationTime) override
    {
        const timespec omit{.tv_sec = 0, .tv_nsec = UTIME_OMIT};
        const timespec times[2] = {accessTime ? *accessTime : omit, modificationTime ? *modificationTime : omit};
        return ::utimensat(AT_FDCWD, local(path).constData(), times, 0) == 0 ? 0 : -errno;
    }

    int freeSpace(quint64 &totalBytes, quint64 &availableBytes) override
    {
        totalBytes = 1000 * 4096;
        availableBytes = 250 * 4096;
        return broken ? -EIO : 0;
    }

private:
    QByteArray local(const QByteArray &path)
    {
        std::lock_guard lock(m_mutex);
        m_seenPaths.append(path);
        QByteArray result = path;
        result.replace('\\', '/');
        return m_root + result;
    }

    int descriptor(quint32 fileId)
    {
        std::lock_guard lock(m_mutex);
        return m_files.value(fileId, -1);
    }

    const QByteArray m_root;
    std::mutex m_mutex;
    QList<QByteArray> m_seenPaths;
    QHash<quint32, int> m_files;
    quint32 m_nextFileId = 1;
};

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

static bool isMountPoint(const QString &path)
{
    // Not QStorageInfo, which asks every file system that is mounted for
    // its size and leaves out the ones that do not have one.
    QFile mounts(u"/proc/self/mountinfo"_s);
    if (!mounts.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray encoded = QFile::encodeName(path);
    const auto lines = mounts.readAll().split('\n');
    return std::any_of(lines.begin(), lines.end(), [&](const QByteArray &line) {
        const auto fields = line.split(' ');
        return fields.size() > 4 && fields.at(4) == encoded;
    });
}

class DriveFilesystemTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void testMountAndUnmount();
    void testFiles();
    void testLargeFile();
    void testDirectories();
    void testRename();
    void testAttributes();
    void testPathsStayInsideTheDrive();
    void testHostileListing();
    void testNoLinks();
    void testSeveralShares();
    void testRemovedWhileOpen();
    void testBrokenBackend();
    void testMountPointInUse();

private:
    // Start a file system with one share, skipping the test without FUSE.
    void mount();

    std::unique_ptr<QTemporaryDir> m_directory;
    // What the drive contains, the directory of the "client".
    QString m_source;
    // Something outside of the drive that must not be reachable.
    QString m_outside;
    QString m_mountPoint;
    // The directory of the share below the mount point.
    QString m_share;
    std::shared_ptr<LocalBackend> m_backend;
    std::unique_ptr<DriveFilesystem> m_filesystem;
};

void DriveFilesystemTest::init()
{
    m_directory = std::make_unique<QTemporaryDir>();
    QVERIFY(m_directory->isValid());
    m_source = m_directory->filePath(u"client/shared"_s);
    m_outside = m_directory->filePath(u"client/secret.txt"_s);
    m_mountPoint = m_directory->filePath(u"drives"_s);
    m_share = m_mountPoint + u"/test"_s;
    QVERIFY(QDir().mkpath(m_source));
    QVERIFY(writeFile(m_outside, "secret"));

    m_backend = std::make_shared<LocalBackend>(m_source);
    m_filesystem = std::make_unique<DriveFilesystem>(m_mountPoint);
}

void DriveFilesystemTest::cleanup()
{
    m_filesystem.reset();
    m_backend.reset();
    // Never delete through a file system that is still mounted.
    QVERIFY(!isMountPoint(m_mountPoint));
    m_directory.reset();
}

void DriveFilesystemTest::mount()
{
    const auto name = m_filesystem->addShare("test", m_backend);
    if (name.isEmpty()) {
        // No /dev/fuse or fusermount3, or not allowed to use them, as in most containers.
        QSKIP("Could not mount a FUSE file system");
    }
    QCOMPARE(name, QByteArray("test"));
}

void DriveFilesystemTest::testMountAndUnmount()
{
    QVERIFY(!m_filesystem->isMounted());
    QVERIFY(!QFileInfo::exists(m_mountPoint));

    mount();
    if (QTest::currentTestResolved()) {
        return;
    }
    QVERIFY(m_filesystem->isMounted());
    QVERIFY(isMountPoint(m_mountPoint));
    QCOMPARE(QDir(m_mountPoint).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{u"test"_s});
    QVERIFY(QFileInfo(m_share).isDir());
    // Only for us.
    QCOMPARE(QFileInfo(m_mountPoint).ownerId(), getuid());

    // Nothing can be added next to the shares, and the shares can not be removed.
    QVERIFY(!writeFile(m_mountPoint + u"/file"_s, "data"));
    QVERIFY(!QDir().mkdir(m_mountPoint + u"/directory"_s));
    QVERIFY(!QDir().rmdir(m_share));
    QVERIFY(!QDir().rename(m_share, m_mountPoint + u"/other"_s));

    m_filesystem->removeShare("test");
    QVERIFY(!m_filesystem->isMounted());
    QVERIFY(!isMountPoint(m_mountPoint));
    // Created by the file system, so removed by it as well.
    QVERIFY(!QFileInfo::exists(m_mountPoint));

    // And again, a new client can connect.
    QCOMPARE(m_filesystem->addShare("test", m_backend), QByteArray("test"));
    QVERIFY(isMountPoint(m_mountPoint));
    QVERIFY(QFileInfo(m_share).isDir());
}

void DriveFilesystemTest::testFiles()
{
    QVERIFY(writeFile(m_source + u"/existing.txt"_s, "from the client"));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QCOMPARE(readFile(m_share + u"/existing.txt"_s), QByteArray("from the client"));
    QCOMPARE(QFileInfo(m_share + u"/existing.txt"_s).size(), 15);
    QVERIFY(!QFileInfo::exists(m_share + u"/missing.txt"_s));

    // Create, the client sees it.
    QVERIFY(writeFile(m_share + u"/new.txt"_s, "from the server"));
    QCOMPARE(readFile(m_source + u"/new.txt"_s), QByteArray("from the server"));
    QCOMPARE(QFileInfo(m_share + u"/new.txt"_s).size(), 15);

    // Overwrite with something shorter.
    QVERIFY(writeFile(m_share + u"/new.txt"_s, "short"));
    QCOMPARE(readFile(m_source + u"/new.txt"_s), QByteArray("short"));
    QCOMPARE(QFileInfo(m_share + u"/new.txt"_s).size(), 5);

    // Append.
    {
        QFile file(m_share + u"/new.txt"_s);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(file.write("er text"), 7);
    }
    QCOMPARE(readFile(m_share + u"/new.txt"_s), QByteArray("shorter text"));

    // The size of a file that is still open is the one we wrote.
    {
        QFile file(m_share + u"/open.txt"_s);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write("0123456789"), 10);
        QVERIFY(file.flush());
        struct stat st;
        QCOMPARE(::stat(QFile::encodeName(file.fileName()).constData(), &st), 0);
        QCOMPARE(st.st_size, 10);
        QVERIFY(file.resize(4));
        QCOMPARE(::stat(QFile::encodeName(file.fileName()).constData(), &st), 0);
        QCOMPARE(st.st_size, 4);
        QVERIFY(file.seek(0));
        QCOMPARE(file.readAll(), QByteArray("0123"));
    }
    QCOMPARE(readFile(m_source + u"/open.txt"_s), QByteArray("0123"));

    // Exclusive creation of something that exists.
    const int descriptor = ::open(QFile::encodeName(m_share + u"/new.txt"_s).constData(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    QCOMPARE(descriptor, -1);
    QCOMPARE(errno, EEXIST);

    // Truncate without opening.
    QCOMPARE(::truncate(QFile::encodeName(m_share + u"/new.txt"_s).constData(), 3), 0);
    QCOMPARE(readFile(m_source + u"/new.txt"_s), QByteArray("sho"));
    QCOMPARE(QFileInfo(m_share + u"/new.txt"_s).size(), 3);

    // Delete.
    QVERIFY(QFile::remove(m_share + u"/new.txt"_s));
    QVERIFY(!QFileInfo::exists(m_source + u"/new.txt"_s));
    QVERIFY(!QFileInfo::exists(m_share + u"/new.txt"_s));
    QVERIFY(!QFile::remove(m_share + u"/new.txt"_s));

    // Changes made by the client show up after a moment.
    QVERIFY(writeFile(m_source + u"/later.txt"_s, "later"));
    QTRY_COMPARE_WITH_TIMEOUT(readFile(m_share + u"/later.txt"_s), QByteArray("later"), 10000);

    // Nothing stays open on the client. Releasing a file is asynchronous.
    QTRY_COMPARE(m_backend->openFileCount(), 0);
}

void DriveFilesystemTest::testLargeFile()
{
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    // Not a multiple of any block size.
    QByteArray data(24 * 1024 * 1024 + 12345, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), data.size() / 4);
    const auto hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    QVERIFY(writeFile(m_share + u"/large.bin"_s, data));
    QCOMPARE(QFileInfo(m_source + u"/large.bin"_s).size(), data.size());
    QCOMPARE(QCryptographicHash::hash(readFile(m_source + u"/large.bin"_s), QCryptographicHash::Sha256), hash);
    QCOMPARE(QFileInfo(m_share + u"/large.bin"_s).size(), data.size());
    QCOMPARE(QCryptographicHash::hash(readFile(m_share + u"/large.bin"_s), QCryptographicHash::Sha256), hash);

    // Read from the middle, across the size of a request.
    QFile file(m_share + u"/large.bin"_s);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.seek(5 * 1024 * 1024 + 7));
    QCOMPARE(file.read(300000), data.mid(5 * 1024 * 1024 + 7, 300000));
    // And past the end.
    QVERIFY(file.seek(data.size() - 10));
    QCOMPARE(file.read(100), data.right(10));
}

void DriveFilesystemTest::testDirectories()
{
    QVERIFY(QDir().mkpath(m_source + u"/music/album"_s));
    QVERIFY(writeFile(m_source + u"/music/album/song.ogg"_s, "la la la"));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QCOMPARE(QDir(m_share).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{u"music"_s});
    QCOMPARE(QDir(m_share + u"/music/album"_s).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{u"song.ogg"_s});
    QVERIFY(QFileInfo(m_share + u"/music"_s).isDir());
    QVERIFY(QFileInfo(m_share + u"/music/album/song.ogg"_s).isFile());
    QCOMPARE(readFile(m_share + u"/music/album/song.ogg"_s), QByteArray("la la la"));
    QVERIFY(!QDir(m_share + u"/missing"_s).exists());

    QVERIFY(QDir().mkdir(m_share + u"/Nová složka"_s));
    QVERIFY(QFileInfo(m_source + u"/Nová složka"_s).isDir());
    QVERIFY(!QDir().mkdir(m_share + u"/Nová složka"_s));
    QVERIFY(writeFile(m_share + u"/Nová složka/soubor"_s, "obsah"));
    QCOMPARE(QDir(m_share).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name), (QStringList{u"Nová složka"_s, u"music"_s}));

    // Not empty. The client is not even asked, some remove what is in the directory.
    QCOMPARE(::rmdir(QFile::encodeName(m_share + u"/Nová složka"_s).constData()), -1);
    QCOMPARE(errno, ENOTEMPTY);
    QCOMPARE(m_backend->removeDirectoryCalls, 0);
    QVERIFY(QFile::remove(m_share + u"/Nová složka/soubor"_s));
    QVERIFY(QDir().rmdir(m_share + u"/Nová složka"_s));
    QVERIFY(!QFileInfo::exists(m_source + u"/Nová složka"_s));
    QVERIFY(!QFileInfo::exists(m_share + u"/Nová složka"_s));

    QVERIFY(QDir(m_share + u"/music"_s).removeRecursively());
    QVERIFY(QDir(m_source).isEmpty());
}

void DriveFilesystemTest::testRename()
{
    QVERIFY(QDir().mkpath(m_source + u"/a"_s));
    QVERIFY(QDir().mkpath(m_source + u"/b"_s));
    QVERIFY(writeFile(m_source + u"/a/one"_s, "one"));
    QVERIFY(writeFile(m_source + u"/a/two"_s, "two"));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QVERIFY(QFile::rename(m_share + u"/a/one"_s, m_share + u"/b/uno"_s));
    QVERIFY(!QFileInfo::exists(m_share + u"/a/one"_s));
    QCOMPARE(readFile(m_share + u"/b/uno"_s), QByteArray("one"));
    QCOMPARE(readFile(m_source + u"/b/uno"_s), QByteArray("one"));

    // Replaces the target, like rename() does.
    QCOMPARE(::rename(QFile::encodeName(m_share + u"/a/two"_s).constData(), QFile::encodeName(m_share + u"/b/uno"_s).constData()), 0);
    QCOMPARE(readFile(m_share + u"/b/uno"_s), QByteArray("two"));
    QVERIFY(QDir(m_source + u"/a"_s).isEmpty());

    // A directory, with what is in it.
    QVERIFY(QDir().rename(m_share + u"/b"_s, m_share + u"/c"_s));
    QVERIFY(!QFileInfo::exists(m_share + u"/b"_s));
    QCOMPARE(readFile(m_share + u"/c/uno"_s), QByteArray("two"));

    // Unless asked not to replace.
    QVERIFY(writeFile(m_share + u"/c/dos"_s, "dos"));
    QCOMPARE(::renameat2(AT_FDCWD,
                         QFile::encodeName(m_share + u"/c/dos"_s).constData(),
                         AT_FDCWD,
                         QFile::encodeName(m_share + u"/c/uno"_s).constData(),
                         RENAME_NOREPLACE),
             -1);
    QCOMPARE(errno, EEXIST);
    QCOMPARE(readFile(m_share + u"/c/uno"_s), QByteArray("two"));
}

void DriveFilesystemTest::testAttributes()
{
    QVERIFY(writeFile(m_source + u"/file"_s, "12345"));
    QVERIFY(writeFile(m_source + u"/protected"_s, "12345"));
    QVERIFY(QFile::setPermissions(m_source + u"/protected"_s, QFile::ReadOwner));
    // Whatever the client has, it is a plain file here.
    QVERIFY(writeFile(m_source + u"/program"_s, "#!/bin/sh\n"));
    QCOMPARE(::chmod(QFile::encodeName(m_source + u"/program"_s).constData(), 06755), 0);
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    struct stat st;
    QCOMPARE(::stat(QFile::encodeName(m_share + u"/file"_s).constData(), &st), 0);
    QCOMPARE(st.st_mode, mode_t(S_IFREG | 0644));
    QCOMPARE(st.st_uid, getuid());
    QCOMPARE(st.st_size, 5);
    QCOMPARE(::stat(QFile::encodeName(m_share + u"/protected"_s).constData(), &st), 0);
    QCOMPARE(st.st_mode, mode_t(S_IFREG | 0444));
    QVERIFY(!writeFile(m_share + u"/protected"_s, "changed"));
    QCOMPARE(::stat(QFile::encodeName(m_share + u"/program"_s).constData(), &st), 0);
    QCOMPARE(st.st_mode, mode_t(S_IFREG | 0644));
    QCOMPARE(::stat(QFile::encodeName(m_share).constData(), &st), 0);
    QCOMPARE(st.st_mode, mode_t(S_IFDIR | 0755));

    // Times, as programs that copy files set them.
    const timespec times[2] = {{.tv_sec = 1000000000, .tv_nsec = 0}, {.tv_sec = 1234567890, .tv_nsec = 0}};
    QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(m_share + u"/file"_s).constData(), times, 0), 0);
    QCOMPARE(::stat(QFile::encodeName(m_source + u"/file"_s).constData(), &st), 0);
    QCOMPARE(st.st_mtim.tv_sec, 1234567890);
    QCOMPARE(::stat(QFile::encodeName(m_share + u"/file"_s).constData(), &st), 0);
    QCOMPARE(st.st_mtim.tv_sec, 1234567890);
    QCOMPARE(st.st_atim.tv_sec, 1000000000);

    // Accepted so that copying files works, but there is nothing to change.
    QCOMPARE(::chmod(QFile::encodeName(m_share + u"/file"_s).constData(), 0600), 0);
    QCOMPARE(::chown(QFile::encodeName(m_share + u"/file"_s).constData(), getuid(), getgid()), 0);

    struct statvfs space;
    QCOMPARE(::statvfs(QFile::encodeName(m_share).constData(), &space), 0);
    QCOMPARE(space.f_blocks * space.f_frsize, 1000 * 4096);
    QCOMPARE(space.f_bavail * space.f_frsize, 250 * 4096);
}

void DriveFilesystemTest::testPathsStayInsideTheDrive()
{
    QVERIFY(QDir().mkpath(m_source + u"/dir"_s));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    // The kernel resolves ".." itself, it ends up at the mount point and
    // above, never at the client.
    QVERIFY(!QFileInfo::exists(m_share + u"/../secret.txt"_s));
    QVERIFY(!QFileInfo::exists(m_share + u"/dir/../../secret.txt"_s));

    // What the kernel passes on as a name, but a client would take as a path.
    const QStringList names{
        u"..\\secret.txt"_s,
        u"dir\\..\\..\\secret.txt"_s,
        u"\\secret.txt"_s,
        u"..\\"_s,
        u".. "_s,
        u"..."_s,
        u"dir\\file"_s,
    };
    for (const auto &name : names) {
        const QByteArray path = QFile::encodeName(m_share + u'/' + name);
        struct stat st;
        QCOMPARE(::stat(path.constData(), &st), -1);
        QCOMPARE(errno, ENOENT);
        QCOMPARE(::open(path.constData(), O_RDONLY), -1);
        QCOMPARE(::open(path.constData(), O_WRONLY | O_CREAT, 0644), -1);
        QCOMPARE(errno, EINVAL);
        QCOMPARE(::mkdir(path.constData(), 0755), -1);
        QCOMPARE(errno, EINVAL);
        QCOMPARE(::unlink(path.constData()), -1);
        QCOMPARE(::rmdir(path.constData()), -1);
        QCOMPARE(::truncate(path.constData(), 0), -1);
        QVERIFY(writeFile(m_share + u"/source"_s, "data"));
        QCOMPARE(::rename(QFile::encodeName(m_share + u"/source"_s).constData(), path.constData()), -1);
        QCOMPARE(errno, EINVAL);
    }
    // Not valid UTF-8, the conversion to UTF-16 would turn it into something else.
    QCOMPARE(::open(QByteArray(QFile::encodeName(m_share) + "/caf\xE9").constData(), O_WRONLY | O_CREAT, 0644), -1);
    QCOMPARE(errno, EINVAL);

    QCOMPARE(readFile(m_outside), QByteArray("secret"));
    const auto seen = m_backend->seenPaths();
    QVERIFY(!seen.isEmpty());
    for (const auto &path : seen) {
        QVERIFY2(path.startsWith('\\'), path.constData());
        QVERIFY2(!path.contains('/'), path.constData());
        const auto components = path.mid(1).split('\\');
        for (const auto &component : components) {
            QVERIFY2(path == "\\" || !component.isEmpty(), path.constData());
            QVERIFY2(!component.startsWith(".."), path.constData());
            QVERIFY2(component != ".", path.constData());
        }
    }
}

void DriveFilesystemTest::testHostileListing()
{
    QVERIFY(writeFile(m_source + u"/good.txt"_s, "good"));
    // The client can not make us show names that lead elsewhere.
    for (const char *name : {"..", ".", "", "../escape", "a/b", "/etc/passwd", "back\\slash", "..\\escape", "caf\xE9", ".. ", "good.txt"}) {
        DriveEntry entry;
        entry.name = name;
        entry.size = 666;
        m_backend->extraEntries.append(entry);
    }
    DriveEntry longName;
    longName.name = QByteArray(300, 'x');
    m_backend->extraEntries.append(longName);
    DriveEntry embeddedNull;
    embeddedNull.name = QByteArray("nul\0l", 5);
    m_backend->extraEntries.append(embeddedNull);
    DriveEntry huge;
    huge.name = "huge";
    huge.size = std::numeric_limits<quint64>::max();
    m_backend->extraEntries.append(huge);

    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QCOMPARE(QDir(m_share).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name),
             (QStringList{u"good.txt"_s, u"huge"_s}));
    // The first one wins, not the duplicate that was added.
    QCOMPARE(QFileInfo(m_share + u"/good.txt"_s).size(), 4);
    QVERIFY(QFileInfo(m_share + u"/huge"_s).size() > 0);

    // A directory with files we do not show is not empty either.
    QVERIFY(QDir().mkdir(m_share + u"/directory"_s));
    QCOMPARE(::rmdir(QFile::encodeName(m_share + u"/directory"_s).constData()), -1);
    QCOMPARE(errno, ENOTEMPTY);
    QCOMPARE(m_backend->removeDirectoryCalls, 0);
    QVERIFY(QFileInfo(m_source + u"/directory"_s).isDir());

    // Exactly what readdir() returns, without anything Qt may filter.
    QList<QByteArray> names;
    DIR *directory = ::opendir(QFile::encodeName(m_share).constData());
    QVERIFY(directory);
    while (const dirent *item = ::readdir(directory)) {
        names.append(item->d_name);
    }
    ::closedir(directory);
    std::sort(names.begin(), names.end());
    QCOMPARE(names, (QList<QByteArray>{".", "..", "directory", "good.txt", "huge"}));
}

void DriveFilesystemTest::testNoLinks()
{
    QVERIFY(writeFile(m_source + u"/file"_s, "data"));
    // A link on the client is the client's business, here it is a file that
    // can not be followed anywhere.
    QVERIFY(QFile::link(m_outside, m_source + u"/link"_s));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QVERIFY(!QFile::link(u"/etc/passwd"_s, m_share + u"/passwd"_s));
    QCOMPARE(::symlink("/etc/passwd", QFile::encodeName(m_share + u"/passwd"_s).constData()), -1);
    QCOMPARE(errno, EPERM);
    QCOMPARE(::link(QFile::encodeName(m_share + u"/file"_s).constData(), QFile::encodeName(m_share + u"/hard"_s).constData()), -1);
    QCOMPARE(errno, EPERM);
    QCOMPARE(::mknod(QFile::encodeName(m_share + u"/fifo"_s).constData(), S_IFIFO | 0644, 0), -1);

    struct stat st;
    QCOMPARE(::lstat(QFile::encodeName(m_share + u"/link"_s).constData(), &st), 0);
    QVERIFY(S_ISREG(st.st_mode));
    QVERIFY(!QFileInfo(m_share + u"/link"_s).isSymLink());
    // The backend of the test refuses to follow links, a real client reads the target.
    QVERIFY(readFile(m_share + u"/link"_s).isEmpty());
}

void DriveFilesystemTest::testSeveralShares()
{
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    const QString otherSource = m_directory->filePath(u"other"_s);
    QVERIFY(QDir().mkpath(otherSource));
    QVERIFY(writeFile(otherSource + u"/other.txt"_s, "other"));
    auto otherBackend = std::make_shared<LocalBackend>(otherSource);

    // The same name as the first one.
    QCOMPARE(m_filesystem->addShare("test", otherBackend), QByteArray("test-2"));
    QCOMPARE(m_filesystem->addShare("documents", otherBackend), QByteArray("documents"));
    QCOMPARE(QDir(m_mountPoint).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name), (QStringList{u"documents"_s, u"test"_s, u"test-2"_s}));
    QCOMPARE(readFile(m_mountPoint + u"/test-2/other.txt"_s), QByteArray("other"));
    QVERIFY(!QFileInfo::exists(m_share + u"/other.txt"_s));

    // Not valid as the name of a directory.
    QVERIFY(m_filesystem->addShare("..", otherBackend).isEmpty());
    QVERIFY(m_filesystem->addShare("a/b", otherBackend).isEmpty());
    QVERIFY(m_filesystem->addShare("", otherBackend).isEmpty());

    // Files can not be moved between drives by renaming them.
    QVERIFY(writeFile(m_share + u"/mine.txt"_s, "mine"));
    QCOMPARE(::rename(QFile::encodeName(m_share + u"/mine.txt"_s).constData(), QFile::encodeName(m_mountPoint + u"/test-2/mine.txt"_s).constData()), -1);
    QCOMPARE(errno, EXDEV);

    m_filesystem->removeShare("test");
    QVERIFY(m_filesystem->isMounted());
    QVERIFY(!QFileInfo::exists(m_share));
    QCOMPARE(readFile(m_mountPoint + u"/test-2/other.txt"_s), QByteArray("other"));

    m_filesystem->removeShare("test-2");
    m_filesystem->removeShare("unknown");
    QVERIFY(m_filesystem->isMounted());
    m_filesystem->removeShare("documents");
    QVERIFY(!m_filesystem->isMounted());
    QVERIFY(!isMountPoint(m_mountPoint));
}

void DriveFilesystemTest::testRemovedWhileOpen()
{
    QVERIFY(writeFile(m_source + u"/file"_s, QByteArray(1024 * 1024, 'x')));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    const int descriptor = ::open(QFile::encodeName(m_share + u"/file"_s).constData(), O_RDWR);
    QVERIFY(descriptor >= 0);
    char buffer[16];
    QCOMPARE(::pread(descriptor, buffer, sizeof(buffer), 0), ssize_t(sizeof(buffer)));

    // The client disconnects. Nothing hangs, and nothing is left mounted.
    m_filesystem->removeShare("test");
    QVERIFY(!isMountPoint(m_mountPoint));
    QCOMPARE(::pread(descriptor, buffer, sizeof(buffer), 512 * 1024), -1);
    QCOMPARE(::pwrite(descriptor, buffer, sizeof(buffer), 0), -1);
    ::close(descriptor);
    QVERIFY(!QFileInfo::exists(m_share));
}

void DriveFilesystemTest::testBrokenBackend()
{
    QVERIFY(writeFile(m_source + u"/file"_s, "data"));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }

    QCOMPARE(readFile(m_share + u"/file"_s), QByteArray("data"));
    m_backend->broken = true;
    // Errors, not hangs or stale data, once the listings have expired.
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(m_share + u"/file"_s), 10000);
    QVERIFY(!writeFile(m_share + u"/new"_s, "data"));
    QVERIFY(QDir(m_share).entryList().isEmpty());

    m_backend->broken = false;
    QTRY_COMPARE_WITH_TIMEOUT(readFile(m_share + u"/file"_s), QByteArray("data"), 10000);
}

void DriveFilesystemTest::testMountPointInUse()
{
    // Do not hide what someone keeps in the directory.
    QVERIFY(QDir().mkpath(m_mountPoint));
    QVERIFY(writeFile(m_mountPoint + u"/precious.txt"_s, "precious"));
    QVERIFY(m_filesystem->addShare("test", m_backend).isEmpty());
    QVERIFY(!m_filesystem->isMounted());
    QVERIFY(!isMountPoint(m_mountPoint));
    QCOMPARE(readFile(m_mountPoint + u"/precious.txt"_s), QByteArray("precious"));

    // Nor mount on something reached through a link.
    QVERIFY(QFile::remove(m_mountPoint + u"/precious.txt"_s));
    QVERIFY(QDir().rmdir(m_mountPoint));
    const QString elsewhere = m_directory->filePath(u"elsewhere"_s);
    QVERIFY(QDir().mkpath(elsewhere));
    QVERIFY(QFile::link(elsewhere, m_mountPoint));
    QVERIFY(m_filesystem->addShare("test", m_backend).isEmpty());
    QVERIFY(!isMountPoint(elsewhere));
    QVERIFY(QFile::remove(m_mountPoint));

    // An existing, empty directory is used and left in place.
    QVERIFY(QDir().mkpath(m_mountPoint));
    mount();
    if (QTest::currentTestResolved()) {
        return;
    }
    QVERIFY(isMountPoint(m_mountPoint));
    m_filesystem->removeShare("test");
    QVERIFY(!isMountPoint(m_mountPoint));
    QVERIFY(QFileInfo(m_mountPoint).isDir());
}

QTEST_GUILESS_MAIN(DriveFilesystemTest)

#include "drivefilesystemtest.moc"
