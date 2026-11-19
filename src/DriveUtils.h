// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

#include <time.h>

#include "krdp_export.h"

/**
 * Helpers for redirected drives ([MS-RDPEFS]) that do not need a connection.
 *
 * Everything that decides which paths are sent to the client, and which names
 * received from the client are shown in the file system, is in here so that
 * it can be tested on its own.
 */
namespace KRdp::DriveUtils
{

/**
 * A path below the mount point, split into the share and the path the client
 * knows the file by.
 */
struct SplitPath {
    /// The first component of the path, empty for the mount point itself.
    QByteArray share;
    /// The remaining path with `\` as separator, `\` for the root of the share.
    QByteArray path;

    bool operator==(const SplitPath &other) const = default;
};

/**
 * Whether a name can be used as a single component of a path sent to the
 * client, and whether a name received from the client can be shown.
 *
 * Refuses everything that could make the client leave the directory the name
 * is in: separators of either kind, names that are or that Windows would
 * normalize to `.` and `..`, and names that are not valid UTF-8 and would be
 * changed by the conversion to UTF-16. Characters that only Windows refuses
 * (`:`, `*`, `?`, ...) are left to the client, they are common in the names of
 * files shared by other systems.
 */
KRDP_EXPORT bool isValidFileName(QByteArrayView name);

/**
 * Convert a path the kernel passed to the file system, `/share/dir/file`,
 * to the share and the path within it, `share` and `\dir\file`.
 *
 * \return Nothing when the path is not absolute or one of its components is
 *         not a valid file name.
 */
KRDP_EXPORT std::optional<SplitPath> splitPath(QByteArrayView path);

/**
 * The directory a path of a share is in, `\` for the root and its entries.
 */
KRDP_EXPORT QByteArray parentPath(QByteArrayView path);

/**
 * The last component of a path of a share, empty for the root.
 */
KRDP_EXPORT QByteArray fileName(QByteArrayView path);

/**
 * The name of a redirected drive as announced by the client.
 *
 * \param preferredDosName The 8 character name of the device, not necessarily
 *                         terminated.
 * \param deviceData The device data of the announcement. Windows clients send
 *                   the full name as UTF-16 here, FreeRDP as 8 bit characters.
 */
KRDP_EXPORT QString announcedDriveName(QByteArrayView preferredDosName, QByteArrayView deviceData);

/**
 * Turn the name of a redirected drive into a name for its directory.
 *
 * The result is always a valid file name: characters that are not allowed are
 * replaced, and names that are unusable as a whole are replaced with a name
 * made from the device ID.
 */
KRDP_EXPORT QByteArray sanitizedShareName(const QString &name, quint32 deviceId);

/**
 * The error number that best describes an NTSTATUS returned by the client,
 * 0 for success.
 */
KRDP_EXPORT int ntStatusToErrno(quint32 status);

/**
 * Convert a Windows FILETIME, 100 ns intervals since 1601, to a timespec.
 * Times that cannot be represented, like the 0 of an unset time, become the
 * epoch.
 */
KRDP_EXPORT timespec fileTimeToTimespec(qint64 fileTime);

/**
 * Convert a timespec to a Windows FILETIME.
 */
KRDP_EXPORT qint64 timespecToFileTime(const timespec &time);

}
