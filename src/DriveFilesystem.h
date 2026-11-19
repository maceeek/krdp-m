// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QByteArray>
#include <QList>
#include <QString>

#include <time.h>

#include "krdp_export.h"

namespace KRdp
{

/**
 * An entry of a directory of a redirected drive.
 */
struct DriveEntry {
    QByteArray name;
    bool directory = false;
    bool readOnly = false;
    quint64 size = 0;
    timespec accessTime{};
    timespec modificationTime{};
    timespec changeTime{};
};

/**
 * The files of one redirected drive.
 *
 * This is what DriveFilesystem needs from a drive to show it as a directory.
 * Paths use `\` as separator and start with one, `\` is the root of the
 * drive. They only contain components DriveUtils::isValidFileName() accepts.
 *
 * All functions block until the operation is complete, are called from
 * several threads at once and return 0 or a negative error number, unless
 * mentioned otherwise.
 */
class KRDP_EXPORT DriveBackend
{
public:
    enum class Access {
        Read,
        Write,
        ReadWrite,
    };

    enum class Disposition {
        Open, ///< Open the file, fail if it does not exist.
        Truncate, ///< Open and truncate the file, fail if it does not exist.
        Create, ///< Create the file, fail if it exists.
        OpenOrCreate, ///< Open the file, create it if it does not exist.
        CreateOrTruncate, ///< Create the file, truncate it if it exists.
    };

    virtual ~DriveBackend();

    /**
     * List the entries of a directory.
     *
     * The entries are not expected to be checked in any way, they may contain
     * `.` and `..` and names that are not valid.
     */
    virtual int list(const QByteArray &path, QList<DriveEntry> &entries) = 0;

    virtual int open(const QByteArray &path, Access access, Disposition disposition, quint32 &fileId) = 0;
    virtual int close(quint32 fileId) = 0;
    /**
     * \return The number of bytes read, less than size only at the end of the
     *         file, or a negative error number.
     */
    virtual int read(quint32 fileId, char *data, quint32 size, quint64 offset) = 0;
    /**
     * \return The number of bytes written or a negative error number.
     */
    virtual int write(quint32 fileId, const char *data, quint32 size, quint64 offset) = 0;

    virtual int createDirectory(const QByteArray &path) = 0;
    virtual int removeDirectory(const QByteArray &path) = 0;
    virtual int removeFile(const QByteArray &path) = 0;
    virtual int rename(const QByteArray &from, const QByteArray &to, bool replace) = 0;
    virtual int resize(const QByteArray &path, quint64 size) = 0;
    /**
     * Set the times of a file or directory, a null pointer leaves a time as
     * it is.
     */
    virtual int setTimes(const QByteArray &path, const timespec *accessTime, const timespec *modificationTime) = 0;
    virtual int freeSpace(quint64 &totalBytes, quint64 &availableBytes) = 0;
};

/**
 * A FUSE file system that shows redirected drives as directories.
 *
 * Each drive, here called a share, is a directory directly below the mount
 * point. The file system is mounted when the first share is added and
 * unmounted again when the last one is removed, so that nothing is left
 * behind while no client redirects a drive.
 *
 * Requests of the kernel are handled by a few threads of the file system,
 * which call into the DriveBackend of the share.
 */
class KRDP_EXPORT DriveFilesystem
{
public:
    /**
     * \param mountPoint The directory to mount the file system on. It is
     *                   created if needed and has to be empty.
     */
    explicit DriveFilesystem(const QString &mountPoint);
    ~DriveFilesystem();

    /**
     * The file system all connections of this process add their drives to,
     * mounted on defaultMountPoint().
     */
    static DriveFilesystem *shared();

    /**
     * `thinclient_drives` in the home directory, the same place xrdp uses.
     */
    static QString defaultMountPoint();

    QString mountPoint() const;
    bool isMounted() const;

    /**
     * Add a share, mounting the file system if it is the first one.
     *
     * \param preferredName The name of the directory, a valid file name. A
     *                      number is appended if it is already in use.
     * \return The name of the directory of the share, empty if the file
     *         system could not be mounted.
     */
    QByteArray addShare(const QByteArray &preferredName, const std::shared_ptr<DriveBackend> &backend);

    /**
     * Remove a share, unmounting the file system if it was the last one.
     *
     * No new calls are made to the backend once this returns and files that
     * are still open fail with an error. Calls that are in progress have to
     * be made to return by the owner of the backend.
     */
    void removeShare(const QByteArray &name);

    /**
     * Detach what a previous process that did not exit cleanly left mounted
     * on a mount point.
     */
    static void cleanUpStaleMount(const QString &mountPoint);

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
