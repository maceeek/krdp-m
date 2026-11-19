// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)

#include "DriveFilesystem.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

#include <fuse.h>
#include <fuse_lowlevel.h>

#include "DriveUtils.h"

#include "krdp_logging.h"

using namespace Qt::StringLiterals;

namespace KRdp
{

using Clock = std::chrono::steady_clock;

// The number of threads that handle requests of the kernel. Each one blocks
// while the client handles a request, so this is how many operations can be
// in flight at once.
static constexpr int WorkerCount = 8;
// How long a directory listing is used to answer lookups. Changes made
// through the file system invalidate the listing right away, this is only
// how long it takes for changes made on the client to show up.
static constexpr auto ListingLifetime = std::chrono::milliseconds(2000);
// Listing a large directory takes a request per entry, listings are kept for
// this many times the time it took to get them so that walking one does not
// list it over and over.
static constexpr int ListingLifetimeFactor = 10;
static constexpr auto MaximumListingLifetime = std::chrono::seconds(60);
// Programs that list a directory expect to see its current contents.
static constexpr auto ReaddirListingLifetime = std::chrono::milliseconds(250);
static constexpr qsizetype MaximumCachedListings = 128;
// Bounds the memory a client can make us use for a single directory.
static constexpr qsizetype MaximumListingEntries = 200000;
static constexpr auto FreeSpaceLifetime = std::chrono::seconds(2);
// The largest read or write sent to the client in one request.
static constexpr quint32 MaximumIoSize = 128 * 1024;
static constexpr double KernelCacheTimeout = 1.0;

DriveBackend::~DriveBackend() = default;

namespace
{

struct Listing {
    QList<DriveEntry> entries;
    QHash<QByteArray, qsizetype> index;
    Clock::time_point time;
    Clock::duration lifetime;

    const DriveEntry *find(const QByteArray &name) const
    {
        const auto it = index.constFind(name);
        return it != index.constEnd() ? &entries.at(*it) : nullptr;
    }
};

// What we know about the size of a file that is open for writing. Windows
// only updates the size in the directory when a file is closed, so the
// listing can not be trusted for these.
struct OpenSize {
    int writers = 0;
    bool known = false;
    // The size was set by truncating the file, so it can be smaller than the
    // size in the listing.
    bool exact = false;
    quint64 size = 0;
};

struct Share {
    QByteArray name;
    std::shared_ptr<DriveBackend> backend;
    std::atomic<bool> removed = false;
    timespec created{};

    // Held while a directory is listed, so that several lookups in one
    // directory result in a single listing.
    std::mutex listMutex;

    // Guards everything below.
    std::mutex mutex;
    QHash<QByteArray, std::shared_ptr<const Listing>> listings;
    // Incremented when listings are invalidated, so that a listing that was
    // requested before a change is not stored after it.
    quint64 generation = 0;
    QHash<QByteArray, OpenSize> openSizes;
    bool freeSpaceKnown = false;
    Clock::time_point freeSpaceTime;
    quint64 totalBytes = 0;
    quint64 availableBytes = 0;

    int listing(const QByteArray &directory, Clock::duration maximumAge, std::shared_ptr<const Listing> &result);
    int stat(const QByteArray &path, DriveEntry &entry);
    void invalidate(const QByteArray &directory);
    void invalidateAll();
    void applyOpenSize(const QByteArray &path, DriveEntry &entry);
};

// What fuse_file_info::fh points to for open files and directories.
struct Handle {
    std::shared_ptr<Share> share;
    QByteArray path;
    quint32 fileId = 0;
    bool writable = false;
    std::atomic<bool> dirty = false;
    // The entry of the file when it was opened, for files that are unlinked
    // while open.
    DriveEntry entry;
    // The contents of an open directory.
    QList<DriveEntry> entries;
};

std::shared_ptr<const Listing>
freshListing(const QHash<QByteArray, std::shared_ptr<const Listing>> &listings, const QByteArray &directory, Clock::duration maximumAge)
{
    const auto it = listings.constFind(directory);
    if (it == listings.constEnd()) {
        return nullptr;
    }
    const auto age = Clock::now() - (*it)->time;
    return age <= std::max(maximumAge, Clock::duration::zero()) && age <= (*it)->lifetime ? *it : nullptr;
}

int Share::listing(const QByteArray &directory, Clock::duration maximumAge, std::shared_ptr<const Listing> &result)
{
    {
        std::lock_guard lock(mutex);
        if ((result = freshListing(listings, directory, maximumAge))) {
            return 0;
        }
    }

    std::lock_guard listLock(listMutex);
    quint64 startGeneration = 0;
    {
        std::lock_guard lock(mutex);
        // Another thread may have listed the directory while we were waiting.
        if ((result = freshListing(listings, directory, maximumAge))) {
            return 0;
        }
        startGeneration = generation;
    }

    if (removed) {
        return -ENODEV;
    }

    const auto start = Clock::now();
    QList<DriveEntry> entries;
    if (const int error = backend->list(directory, entries); error != 0) {
        return error;
    }

    auto listing = std::make_shared<Listing>();
    listing->time = start;
    listing->lifetime = std::clamp<Clock::duration>((Clock::now() - start) * ListingLifetimeFactor, ListingLifetime, MaximumListingLifetime);
    listing->entries.reserve(entries.size());
    for (auto &entry : entries) {
        // Never show a name that we would not send back to the client,
        // above all not the ones that lead out of the directory.
        if (!DriveUtils::isValidFileName(entry.name) || listing->index.contains(entry.name)) {
            continue;
        }
        if (listing->entries.size() >= MaximumListingEntries) {
            qCWarning(KRDP) << "Directory of redirected drive" << name << "has too many entries, ignoring the rest";
            break;
        }
        listing->index.insert(entry.name, listing->entries.size());
        listing->entries.append(std::move(entry));
    }

    {
        std::lock_guard lock(mutex);
        if (generation == startGeneration) {
            if (listings.size() >= MaximumCachedListings) {
                listings.clear();
            }
            listings.insert(directory, listing);
        }
    }

    result = std::move(listing);
    return 0;
}

void Share::applyOpenSize(const QByteArray &path, DriveEntry &entry)
{
    std::lock_guard lock(mutex);
    const auto it = openSizes.constFind(path);
    if (it != openSizes.constEnd() && it->known) {
        entry.size = it->exact ? it->size : std::max(entry.size, it->size);
    }
}

int Share::stat(const QByteArray &path, DriveEntry &entry)
{
    if (path == "\\") {
        entry = DriveEntry{};
        entry.name = name;
        entry.directory = true;
        entry.accessTime = entry.modificationTime = entry.changeTime = created;
        return 0;
    }

    std::shared_ptr<const Listing> parent;
    if (const int error = listing(DriveUtils::parentPath(path), MaximumListingLifetime, parent); error != 0) {
        return error;
    }
    const auto found = parent->find(DriveUtils::fileName(path));
    if (!found) {
        return -ENOENT;
    }
    entry = *found;
    if (!entry.directory) {
        applyOpenSize(path, entry);
    }
    return 0;
}

void Share::invalidate(const QByteArray &directory)
{
    std::lock_guard lock(mutex);
    ++generation;
    listings.remove(directory);
}

void Share::invalidateAll()
{
    std::lock_guard lock(mutex);
    ++generation;
    listings.clear();
}

}

class KRDP_NO_EXPORT DriveFilesystem::Private
{
public:
    QString mountPoint;
    const uid_t uid = getuid();
    const gid_t gid = getgid();

    // Serializes mounting, unmounting and changes to the shares.
    std::mutex mountMutex;
    fuse *fuseHandle = nullptr;
    int stopEvent = -1;
    bool createdMountPoint = false;
    timespec mountTime{};
    std::atomic<bool> stopping = false;
    // The file system was unmounted by someone else.
    std::atomic<bool> lost = false;
    std::vector<std::thread> workers;

    // Guards shares, which the worker threads read.
    mutable std::mutex sharesMutex;
    QHash<QByteArray, std::shared_ptr<Share>> shares;

    // The open files and directories, to free the ones that are still open
    // when the file system is unmounted.
    std::mutex handlesMutex;
    QSet<Handle *> handles;

    bool mount();
    void unmount();
    void work();

    std::shared_ptr<Share> findShare(const QByteArray &name) const;
    // Resolve a path of the file system to a share and the path within it.
    // Returns 0 with a null share for the mount point itself.
    int resolve(const char *path, std::shared_ptr<Share> &share, QByteArray &sharePath) const;
    // Resolve the path of a file or directory of a share that an operation
    // is going to use, which the mount point and the shares themselves are not.
    int resolveInShare(const char *path, std::shared_ptr<Share> &share, QByteArray &sharePath) const;
    void fillStat(struct stat *st, const DriveEntry &entry, bool unlinked = false) const;
    void fillRootStat(struct stat *st) const;

    static Private *self()
    {
        return static_cast<Private *>(fuse_get_context()->private_data);
    }
    static Handle *handle(const fuse_file_info *info)
    {
        return info ? reinterpret_cast<Handle *>(info->fh) : nullptr;
    }
    void keepHandle(std::unique_ptr<Handle> &&file, fuse_file_info *info)
    {
        std::lock_guard lock(handlesMutex);
        handles.insert(file.get());
        info->fh = reinterpret_cast<quintptr>(file.release());
    }
    std::unique_ptr<Handle> takeHandle(fuse_file_info *info)
    {
        std::unique_ptr<Handle> file(handle(info));
        info->fh = 0;
        std::lock_guard lock(handlesMutex);
        handles.remove(file.get());
        return file;
    }

    static void *init(fuse_conn_info *connection, fuse_config *config);
    static int getattr(const char *path, struct stat *st, fuse_file_info *info);
    static int mkdir(const char *path, mode_t mode);
    static int unlink(const char *path);
    static int rmdir(const char *path);
    static int symlink(const char *target, const char *path);
    static int link(const char *from, const char *to);
    static int rename(const char *from, const char *to, unsigned int flags);
    static int chmod(const char *path, mode_t mode, fuse_file_info *info);
    static int chown(const char *path, uid_t uid, gid_t gid, fuse_file_info *info);
    static int truncate(const char *path, off_t size, fuse_file_info *info);
    static int open(const char *path, fuse_file_info *info);
    static int create(const char *path, mode_t mode, fuse_file_info *info);
    static int read(const char *path, char *buffer, size_t size, off_t offset, fuse_file_info *info);
    static int write(const char *path, const char *buffer, size_t size, off_t offset, fuse_file_info *info);
    static int statfs(const char *path, struct statvfs *st);
    static int flush(const char *path, fuse_file_info *info);
    static int release(const char *path, fuse_file_info *info);
    static int fsync(const char *path, int dataSync, fuse_file_info *info);
    static int opendir(const char *path, fuse_file_info *info);
    static int readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, fuse_file_info *info, fuse_readdir_flags flags);
    static int releasedir(const char *path, fuse_file_info *info);
    static int utimens(const char *path, const timespec times[2], fuse_file_info *info);

    static int openFile(const char *path, fuse_file_info *info, bool create);
    static const fuse_operations operations;
};

const fuse_operations DriveFilesystem::Private::operations = {
    .getattr = getattr,
    .mkdir = mkdir,
    .unlink = unlink,
    .rmdir = rmdir,
    .symlink = symlink,
    .rename = rename,
    .link = link,
    .chmod = chmod,
    .chown = chown,
    .truncate = truncate,
    .open = open,
    .read = read,
    .write = write,
    .statfs = statfs,
    .flush = flush,
    .release = release,
    .fsync = fsync,
    .opendir = opendir,
    .readdir = readdir,
    .releasedir = releasedir,
    .init = init,
    .create = create,
    .utimens = utimens,
};

std::shared_ptr<Share> DriveFilesystem::Private::findShare(const QByteArray &name) const
{
    std::lock_guard lock(sharesMutex);
    return shares.value(name);
}

int DriveFilesystem::Private::resolve(const char *path, std::shared_ptr<Share> &share, QByteArray &sharePath) const
{
    share.reset();
    // Names that can not be sent to the client do not exist.
    const auto split = path ? DriveUtils::splitPath(path) : std::nullopt;
    if (!split) {
        return -ENOENT;
    }
    if (split->share.isEmpty()) {
        return 0;
    }
    share = findShare(split->share);
    if (!share) {
        return -ENOENT;
    }
    sharePath = split->path;
    return 0;
}

void DriveFilesystem::Private::fillStat(struct stat *st, const DriveEntry &entry, bool unlinked) const
{
    *st = {};
    st->st_uid = uid;
    st->st_gid = gid;
    if (entry.directory) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        // Never a link, a device or executable, whatever the client says.
        st->st_mode = S_IFREG | (entry.readOnly ? 0444 : 0644);
        st->st_nlink = unlinked ? 0 : 1;
        st->st_size = off_t(std::min<quint64>(entry.size, std::numeric_limits<off_t>::max()));
        st->st_blocks = (st->st_size + 511) / 512;
    }
    st->st_blksize = 4096;
    st->st_atim = entry.accessTime;
    st->st_mtim = entry.modificationTime;
    st->st_ctim = entry.changeTime;
}

void DriveFilesystem::Private::fillRootStat(struct stat *st) const
{
    *st = {};
    st->st_uid = uid;
    st->st_gid = gid;
    // Only shares can be added to the mount point, and not through the file system.
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_blksize = 4096;
    st->st_atim = st->st_mtim = st->st_ctim = mountTime;
}

void *DriveFilesystem::Private::init(fuse_conn_info *connection, fuse_config *config)
{
    connection->max_write = MaximumIoSize;

    config->entry_timeout = KernelCacheTimeout;
    config->attr_timeout = KernelCacheTimeout;
    config->negative_timeout = 0;
    // Do not rename files that are deleted while open to hidden files, the
    // client would be left with those if the connection is lost.
    config->hard_remove = 1;
    // Open files are identified by their handle, they may not have a path anymore.
    config->nullpath_ok = 1;
    return fuse_get_context()->private_data;
}

int DriveFilesystem::Private::getattr(const char *path, struct stat *st, fuse_file_info *info)
{
    auto d = self();

    if (auto file = handle(info)) {
        if (!file->share) {
            d->fillRootStat(st);
            return 0;
        }
        DriveEntry entry;
        const int error = file->share->stat(file->path, entry);
        if (error == -ENOENT) {
            // Deleted while open.
            entry = file->entry;
            file->share->applyOpenSize(file->path, entry);
            d->fillStat(st, entry, true);
            return 0;
        }
        if (error == 0) {
            d->fillStat(st, entry);
        }
        return error;
    }

    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = d->resolve(path, share, sharePath); error != 0) {
        return error;
    }
    if (!share) {
        d->fillRootStat(st);
        return 0;
    }

    DriveEntry entry;
    if (const int error = share->stat(sharePath, entry); error != 0) {
        return error;
    }
    d->fillStat(st, entry);
    return 0;
}

int DriveFilesystem::Private::mkdir(const char *path, mode_t)
{
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }
    const int error = share->backend->createDirectory(sharePath);
    share->invalidate(DriveUtils::parentPath(sharePath));
    return error;
}

int DriveFilesystem::Private::unlink(const char *path)
{
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }
    const int error = share->backend->removeFile(sharePath);
    share->invalidate(DriveUtils::parentPath(sharePath));
    return error;
}

int DriveFilesystem::Private::rmdir(const char *path)
{
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }
    // Clients do not agree on what removing a directory that is not empty
    // does: FreeRDP removes everything in it, Windows silently keeps it. So
    // look first, at everything the client has and not only at what we show.
    QList<DriveEntry> entries;
    if (const int error = share->backend->list(sharePath, entries); error != 0) {
        return error;
    }
    const bool empty = std::all_of(entries.cbegin(), entries.cend(), [](const DriveEntry &entry) {
        return entry.name == "." || entry.name == "..";
    });
    if (!empty) {
        return -ENOTEMPTY;
    }

    const int error = share->backend->removeDirectory(sharePath);
    share->invalidateAll();
    return error;
}

int DriveFilesystem::Private::symlink(const char *, const char *)
{
    // Links are not part of the protocol. More importantly, never showing
    // any means a client can not make programs on this side follow one.
    return -EPERM;
}

int DriveFilesystem::Private::link(const char *, const char *)
{
    return -EPERM;
}

int DriveFilesystem::Private::rename(const char *from, const char *to, unsigned int flags)
{
    if (flags & ~RENAME_NOREPLACE) {
        return -EINVAL;
    }

    auto d = self();
    std::shared_ptr<Share> fromShare;
    std::shared_ptr<Share> toShare;
    QByteArray fromPath;
    QByteArray toPath;
    if (const int error = d->resolveInShare(from, fromShare, fromPath); error != 0) {
        return error;
    }
    if (const int error = d->resolveInShare(to, toShare, toPath); error != 0) {
        return error;
    }
    if (fromShare != toShare) {
        return -EXDEV;
    }

    const int error = fromShare->backend->rename(fromPath, toPath, !(flags & RENAME_NOREPLACE));
    fromShare->invalidateAll();
    if (error == 0) {
        std::lock_guard lock(fromShare->mutex);
        if (const auto it = fromShare->openSizes.constFind(fromPath); it != fromShare->openSizes.constEnd()) {
            const auto openSize = *it;
            fromShare->openSizes.erase(it);
            fromShare->openSizes.insert(toPath, openSize);
        }
    }
    return error;
}

int DriveFilesystem::Private::chmod(const char *, mode_t, fuse_file_info *)
{
    // There are no permissions to change, but failing would make copying
    // files to a drive fail for most programs.
    return 0;
}

int DriveFilesystem::Private::chown(const char *, uid_t uid, gid_t gid, fuse_file_info *)
{
    auto d = self();
    const bool sameUser = uid == uid_t(-1) || uid == d->uid;
    const bool sameGroup = gid == gid_t(-1) || gid == d->gid;
    return sameUser && sameGroup ? 0 : -EPERM;
}

int DriveFilesystem::Private::truncate(const char *path, off_t size, fuse_file_info *info)
{
    if (size < 0) {
        return -EINVAL;
    }

    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (auto file = handle(info); file && file->share) {
        share = file->share;
        sharePath = file->path;
    } else if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }
    if (share->removed) {
        return -ENODEV;
    }

    const int error = share->backend->resize(sharePath, quint64(size));
    if (error == 0) {
        std::lock_guard lock(share->mutex);
        if (auto it = share->openSizes.find(sharePath); it != share->openSizes.end()) {
            it->known = true;
            it->exact = true;
            it->size = quint64(size);
        }
    }
    share->invalidate(DriveUtils::parentPath(sharePath));
    return error;
}

int DriveFilesystem::Private::openFile(const char *path, fuse_file_info *info, bool create)
{
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }

    DriveBackend::Access access = DriveBackend::Access::Read;
    switch (info->flags & O_ACCMODE) {
    case O_WRONLY:
        access = DriveBackend::Access::Write;
        break;
    case O_RDWR:
        access = DriveBackend::Access::ReadWrite;
        break;
    default:
        break;
    }
    const bool writable = access != DriveBackend::Access::Read;
    const bool truncate = (info->flags & O_TRUNC) && writable;

    DriveBackend::Disposition disposition = DriveBackend::Disposition::Open;
    if (create) {
        if (info->flags & O_EXCL) {
            disposition = DriveBackend::Disposition::Create;
        } else {
            disposition = truncate ? DriveBackend::Disposition::CreateOrTruncate : DriveBackend::Disposition::OpenOrCreate;
        }
    } else if (truncate) {
        disposition = DriveBackend::Disposition::Truncate;
    }

    auto file = std::make_unique<Handle>();
    file->share = share;
    file->path = sharePath;
    file->writable = writable;
    if (create) {
        clock_gettime(CLOCK_REALTIME, &file->entry.modificationTime);
        file->entry.accessTime = file->entry.changeTime = file->entry.modificationTime;
        file->entry.name = DriveUtils::fileName(sharePath);
    } else if (const int error = share->stat(sharePath, file->entry); error != 0) {
        return error;
    } else if (file->entry.directory) {
        return -EISDIR;
    }

    const int error = share->backend->open(sharePath, access, disposition, file->fileId);
    if (create || truncate) {
        share->invalidate(DriveUtils::parentPath(sharePath));
    }
    if (error != 0) {
        return error;
    }

    if (writable) {
        std::lock_guard lock(share->mutex);
        auto &openSize = share->openSizes[sharePath];
        ++openSize.writers;
        if (truncate || disposition == DriveBackend::Disposition::Create) {
            openSize.known = true;
            openSize.exact = true;
            openSize.size = 0;
        }
    }

    self()->keepHandle(std::move(file), info);
    return 0;
}

int DriveFilesystem::Private::open(const char *path, fuse_file_info *info)
{
    return openFile(path, info, false);
}

int DriveFilesystem::Private::create(const char *path, mode_t, fuse_file_info *info)
{
    return openFile(path, info, true);
}

int DriveFilesystem::Private::read(const char *, char *buffer, size_t size, off_t offset, fuse_file_info *info)
{
    auto file = handle(info);
    if (!file || !file->share || offset < 0) {
        return -EBADF;
    }

    size = std::min<size_t>(size, std::numeric_limits<int>::max());
    size_t total = 0;
    while (total < size) {
        if (file->share->removed) {
            return -ENODEV;
        }
        const quint32 chunk = quint32(std::min<size_t>(size - total, MaximumIoSize));
        const int result = file->share->backend->read(file->fileId, buffer + total, chunk, quint64(offset) + total);
        if (result < 0) {
            return result;
        }
        total += std::min<size_t>(size_t(result), chunk);
        if (quint32(result) < chunk) {
            // The end of the file.
            break;
        }
    }
    return int(total);
}

int DriveFilesystem::Private::write(const char *, const char *buffer, size_t size, off_t offset, fuse_file_info *info)
{
    auto file = handle(info);
    if (!file || !file->share || offset < 0) {
        return -EBADF;
    }

    size = std::min<size_t>(size, std::numeric_limits<int>::max());
    size_t total = 0;
    int error = 0;
    while (total < size) {
        if (file->share->removed) {
            error = -ENODEV;
            break;
        }
        const quint32 chunk = quint32(std::min<size_t>(size - total, MaximumIoSize));
        const int result = file->share->backend->write(file->fileId, buffer + total, chunk, quint64(offset) + total);
        if (result < 0) {
            error = result;
            break;
        }
        if (result == 0) {
            error = -EIO;
            break;
        }
        total += std::min<size_t>(size_t(result), chunk);
    }

    if (total > 0) {
        file->dirty = true;
        std::lock_guard lock(file->share->mutex);
        if (auto it = file->share->openSizes.find(file->path); it != file->share->openSizes.end()) {
            it->known = true;
            it->size = std::max(it->size, quint64(offset) + total);
        }
    }
    return total > 0 ? int(total) : error;
}

int DriveFilesystem::Private::statfs(const char *path, struct statvfs *st)
{
    static constexpr quint64 BlockSize = 4096;

    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = self()->resolve(path, share, sharePath); error != 0) {
        return error;
    }

    *st = {};
    st->f_bsize = BlockSize;
    st->f_frsize = BlockSize;
    st->f_namemax = NAME_MAX;
    if (!share) {
        return 0;
    }

    quint64 totalBytes = 0;
    quint64 availableBytes = 0;
    {
        std::unique_lock lock(share->mutex);
        if (share->freeSpaceKnown && Clock::now() - share->freeSpaceTime <= FreeSpaceLifetime) {
            totalBytes = share->totalBytes;
            availableBytes = share->availableBytes;
        } else {
            lock.unlock();
            if (share->removed) {
                return -ENODEV;
            }
            if (const int error = share->backend->freeSpace(totalBytes, availableBytes); error != 0) {
                return error;
            }
            lock.lock();
            share->freeSpaceKnown = true;
            share->freeSpaceTime = Clock::now();
            share->totalBytes = totalBytes;
            share->availableBytes = availableBytes;
        }
    }

    st->f_blocks = totalBytes / BlockSize;
    st->f_bfree = std::min(availableBytes, totalBytes) / BlockSize;
    st->f_bavail = st->f_bfree;
    return 0;
}

static void invalidateWritten(Handle *file)
{
    if (file->writable && file->dirty.exchange(false)) {
        file->share->invalidate(DriveUtils::parentPath(file->path));
    }
}

int DriveFilesystem::Private::flush(const char *, fuse_file_info *info)
{
    // Called when a program closes the file, the release follows later and
    // the program does not wait for it.
    if (auto file = handle(info); file && file->share) {
        invalidateWritten(file);
    }
    return 0;
}

int DriveFilesystem::Private::release(const char *, fuse_file_info *info)
{
    const auto file = self()->takeHandle(info);
    if (!file || !file->share) {
        return 0;
    }

    if (!file->share->removed) {
        file->share->backend->close(file->fileId);
    }
    if (file->writable) {
        std::lock_guard lock(file->share->mutex);
        if (auto it = file->share->openSizes.find(file->path); it != file->share->openSizes.end() && --it->writers <= 0) {
            file->share->openSizes.erase(it);
        }
    }
    // The client only now updates the size and times in the directory.
    file->dirty = file->writable;
    invalidateWritten(file.get());
    return 0;
}

int DriveFilesystem::Private::fsync(const char *, int, fuse_file_info *)
{
    // The protocol has no request to flush, writes are not buffered here.
    return 0;
}

int DriveFilesystem::Private::opendir(const char *path, fuse_file_info *info)
{
    auto d = self();
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (const int error = d->resolve(path, share, sharePath); error != 0) {
        return error;
    }

    auto directory = std::make_unique<Handle>();
    if (!share) {
        std::lock_guard lock(d->sharesMutex);
        for (const auto &entry : std::as_const(d->shares)) {
            DriveEntry shareEntry;
            shareEntry.name = entry->name;
            shareEntry.directory = true;
            shareEntry.accessTime = shareEntry.modificationTime = shareEntry.changeTime = entry->created;
            directory->entries.append(shareEntry);
        }
        std::sort(directory->entries.begin(), directory->entries.end(), [](const DriveEntry &first, const DriveEntry &second) {
            return first.name < second.name;
        });
    } else {
        std::shared_ptr<const Listing> listing;
        if (const int error = share->listing(sharePath, ReaddirListingLifetime, listing); error != 0) {
            return error;
        }
        directory->share = share;
        directory->path = sharePath;
        directory->entries = listing->entries;
    }

    d->keepHandle(std::move(directory), info);
    return 0;
}

int DriveFilesystem::Private::readdir(const char *, void *buffer, fuse_fill_dir_t filler, off_t, fuse_file_info *info, fuse_readdir_flags flags)
{
    const auto fillFlags = (flags & FUSE_READDIR_PLUS) ? FUSE_FILL_DIR_PLUS : fuse_fill_dir_flags(0);
    auto d = self();
    auto directory = handle(info);
    if (!directory) {
        return -EBADF;
    }

    filler(buffer, ".", nullptr, 0, fuse_fill_dir_flags(0));
    filler(buffer, "..", nullptr, 0, fuse_fill_dir_flags(0));
    for (const auto &entry : std::as_const(directory->entries)) {
        struct stat st;
        DriveEntry current = entry;
        if (directory->share && !current.directory) {
            QByteArray path = directory->path;
            if (!path.endsWith('\\')) {
                path += '\\';
            }
            path += current.name;
            directory->share->applyOpenSize(path, current);
        }
        d->fillStat(&st, current);
        if (filler(buffer, entry.name.constData(), &st, 0, fillFlags) != 0) {
            return -ENOMEM;
        }
    }
    return 0;
}

int DriveFilesystem::Private::releasedir(const char *, fuse_file_info *info)
{
    self()->takeHandle(info);
    return 0;
}

int DriveFilesystem::Private::utimens(const char *path, const timespec times[2], fuse_file_info *info)
{
    std::shared_ptr<Share> share;
    QByteArray sharePath;
    if (auto file = handle(info); file && file->share) {
        share = file->share;
        sharePath = file->path;
    } else if (const int error = self()->resolveInShare(path, share, sharePath); error != 0) {
        return error;
    }
    if (share->removed) {
        return -ENODEV;
    }
    if (sharePath == "\\") {
        return -EACCES;
    }

    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    timespec values[2];
    const timespec *pointers[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        if (!times || times[i].tv_nsec == UTIME_NOW) {
            values[i] = now;
        } else if (times[i].tv_nsec == UTIME_OMIT) {
            continue;
        } else {
            values[i] = times[i];
        }
        pointers[i] = &values[i];
    }
    if (!pointers[0] && !pointers[1]) {
        return 0;
    }

    const int error = share->backend->setTimes(sharePath, pointers[0], pointers[1]);
    share->invalidate(DriveUtils::parentPath(sharePath));
    return error;
}

int DriveFilesystem::Private::resolveInShare(const char *path, std::shared_ptr<Share> &share, QByteArray &sharePath) const
{
    // Unlike for lookups, say why a name can not be used.
    if (!path || !DriveUtils::splitPath(path)) {
        return -EINVAL;
    }
    if (const int error = resolve(path, share, sharePath); error != 0) {
        return error;
    }
    if (!share || sharePath == "\\") {
        return -EACCES;
    }
    if (share->removed) {
        return -ENODEV;
    }
    return 0;
}

void DriveFilesystem::cleanUpStaleMount(const QString &mountPoint)
{
    struct stat st;
    if (::lstat(QFile::encodeName(mountPoint).constData(), &st) == 0 || errno != ENOTCONN) {
        return;
    }

    // The process serving it is gone.
    qCInfo(KRDP) << "Detaching the stale file system of redirected drives on" << mountPoint;
    QProcess::execute(u"fusermount3"_s, {u"-u"_s, u"-q"_s, u"-z"_s, u"--"_s, mountPoint});
    // It was created for the file system, there is nothing in it.
    ::rmdir(QFile::encodeName(mountPoint).constData());
}

bool DriveFilesystem::Private::mount()
{
    const QByteArray path = QFile::encodeName(mountPoint);

    cleanUpStaleMount(mountPoint);

    createdMountPoint = ::mkdir(path.constData(), 0700) == 0;
    if (!createdMountPoint && errno != EEXIST) {
        qCWarning(KRDP) << "Could not create" << mountPoint << "for redirected drives:" << strerror(errno);
        return false;
    }

    // Do not mount on something we were pointed to through a link, and do
    // not hide files that are there already.
    struct stat st;
    if (::lstat(path.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != uid) {
        qCWarning(KRDP) << "Not mounting redirected drives," << mountPoint << "is not a directory of this user";
        return false;
    }
    if (!QDir(mountPoint).isEmpty()) {
        qCWarning(KRDP) << "Not mounting redirected drives," << mountPoint << "is not empty or in use by another server";
        return false;
    }

    fuse_args arguments = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&arguments, "krdpserver");
    fuse_opt_add_arg(&arguments, "-o");
    fuse_opt_add_arg(&arguments, "fsname=krdp,subtype=krdp,default_permissions,nosuid,nodev,noatime");
    fuseHandle = fuse_new(&arguments, &operations, sizeof(operations), this);
    fuse_opt_free_args(&arguments);
    if (!fuseHandle) {
        qCWarning(KRDP) << "Could not create the file system for redirected drives";
        return false;
    }

    if (fuse_mount(fuseHandle, path.constData()) != 0) {
        qCWarning(KRDP) << "Could not mount the file system for redirected drives on" << mountPoint;
        fuse_destroy(fuseHandle);
        fuseHandle = nullptr;
        if (createdMountPoint) {
            ::rmdir(path.constData());
        }
        return false;
    }

    // The worker threads wait for a request or for us to stop them, which
    // the blocking read of FUSE's own loops can not do.
    const int fuseDescriptor = fuse_session_fd(fuse_get_session(fuseHandle));
    ::fcntl(fuseDescriptor, F_SETFL, ::fcntl(fuseDescriptor, F_GETFL) | O_NONBLOCK);
    clock_gettime(CLOCK_REALTIME, &mountTime);
    stopEvent = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    stopping = false;
    lost = false;
    for (int i = 0; i < WorkerCount; ++i) {
        auto &worker = workers.emplace_back(&Private::work, this);
        pthread_setname_np(worker.native_handle(), "krdp_drive");
    }

    qCInfo(KRDP) << "Mounted the file system for redirected drives on" << mountPoint;
    return true;
}

void DriveFilesystem::Private::unmount()
{
    if (!fuseHandle) {
        return;
    }

    stopping = true;
    const quint64 one = 1;
    if (::write(stopEvent, &one, sizeof(one)) < 0) {
        qCWarning(KRDP) << "Could not wake the threads of the file system for redirected drives:" << strerror(errno);
    }
    for (auto &worker : workers) {
        worker.join();
    }
    workers.clear();

    // Detaches the file system, programs that still have files open get
    // errors from then on.
    fuse_unmount(fuseHandle);
    fuse_destroy(fuseHandle);
    fuseHandle = nullptr;
    ::close(stopEvent);
    stopEvent = -1;
    {
        std::lock_guard lock(handlesMutex);
        qDeleteAll(handles);
        handles.clear();
    }

    if (createdMountPoint) {
        ::rmdir(QFile::encodeName(mountPoint).constData());
        createdMountPoint = false;
    }
    qCInfo(KRDP) << "Unmounted the file system for redirected drives from" << mountPoint;
}

void DriveFilesystem::Private::work()
{
    auto session = fuse_get_session(fuseHandle);
    fuse_buf buffer{};
    std::array<pollfd, 2> descriptors{pollfd{.fd = fuse_session_fd(session), .events = POLLIN, .revents = 0},
                                      pollfd{.fd = stopEvent, .events = POLLIN, .revents = 0}};

    while (!stopping) {
        if (::poll(descriptors.data(), descriptors.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (stopping || descriptors[1].revents != 0) {
            break;
        }

        const int result = fuse_session_receive_buf(session, &buffer);
        if (result == -EINTR || result == -EAGAIN) {
            // Another thread was faster.
            continue;
        }
        if (result <= 0) {
            // Unmounted behind our back.
            if (!stopping && !lost.exchange(true)) {
                qCWarning(KRDP) << "The file system for redirected drives on" << mountPoint << "was unmounted";
            }
            break;
        }
        fuse_session_process_buf(session, &buffer);
    }

    free(buffer.mem);
}

DriveFilesystem::DriveFilesystem(const QString &mountPoint)
    : d(std::make_unique<Private>())
{
    d->mountPoint = mountPoint;
}

DriveFilesystem::~DriveFilesystem()
{
    std::lock_guard lock(d->mountMutex);
    {
        std::lock_guard sharesLock(d->sharesMutex);
        for (const auto &share : std::as_const(d->shares)) {
            share->removed = true;
        }
        d->shares.clear();
    }
    d->unmount();
}

DriveFilesystem *DriveFilesystem::shared()
{
    static DriveFilesystem instance(defaultMountPoint());
    return &instance;
}

QString DriveFilesystem::defaultMountPoint()
{
    return QDir::homePath() + u"/thinclient_drives"_s;
}

QString DriveFilesystem::mountPoint() const
{
    return d->mountPoint;
}

bool DriveFilesystem::isMounted() const
{
    std::lock_guard lock(d->mountMutex);
    return d->fuseHandle && !d->lost;
}

QByteArray DriveFilesystem::addShare(const QByteArray &preferredName, const std::shared_ptr<DriveBackend> &backend)
{
    if (!DriveUtils::isValidFileName(preferredName) || !backend) {
        return {};
    }

    std::lock_guard lock(d->mountMutex);
    if (d->fuseHandle && d->lost) {
        // Someone unmounted the file system, start over.
        d->unmount();
    }
    if (!d->fuseHandle && !d->mount()) {
        return {};
    }

    auto share = std::make_shared<Share>();
    share->backend = backend;
    clock_gettime(CLOCK_REALTIME, &share->created);

    std::lock_guard sharesLock(d->sharesMutex);
    share->name = preferredName;
    for (int suffix = 2; d->shares.contains(share->name); ++suffix) {
        share->name = preferredName + '-' + QByteArray::number(suffix);
    }
    d->shares.insert(share->name, share);
    return share->name;
}

void DriveFilesystem::removeShare(const QByteArray &name)
{
    std::lock_guard lock(d->mountMutex);
    bool empty = false;
    {
        std::lock_guard sharesLock(d->sharesMutex);
        if (const auto share = d->shares.take(name)) {
            share->removed = true;
        }
        empty = d->shares.isEmpty();
    }
    if (empty) {
        d->unmount();
    } else if (d->fuseHandle) {
        // The kernel remembers the directory for a moment, make it forget.
        fuse_lowlevel_notify_inval_entry(fuse_get_session(d->fuseHandle), FUSE_ROOT_ID, name.constData(), size_t(name.size()));
    }
}

}
