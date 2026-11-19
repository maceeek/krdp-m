// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DriveUtils.h"

#include <cerrno>
#include <climits>

#include <QStringDecoder>

#include <winpr/nt.h>

namespace KRdp::DriveUtils
{

// The number of 100 ns intervals between 1601-01-01 and 1970-01-01.
static constexpr qint64 FileTimeEpochOffset = 116444736000000000LL;
static constexpr qint64 FileTimeTicksPerSecond = 10000000LL;

bool isValidFileName(QByteArrayView name)
{
    if (name.isEmpty() || name.size() > NAME_MAX) {
        return false;
    }

    bool onlyDotsAndSpaces = true;
    for (const char character : name) {
        if (character == '/' || character == '\\' || character == '\0') {
            return false;
        }
        if (character != '.' && character != ' ') {
            onlyDotsAndSpaces = false;
        }
    }
    // "." and "..", and what Windows makes of names like ".. " and "...".
    if (onlyDotsAndSpaces) {
        return false;
    }

    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString decoded = decoder.decode(name);
    return !decoder.hasError() && !decoded.contains(QChar::ReplacementCharacter) && !decoded.contains(QChar::Null);
}

std::optional<SplitPath> splitPath(QByteArrayView path)
{
    if (!path.startsWith('/')) {
        return std::nullopt;
    }

    SplitPath result;
    qsizetype start = 1;
    while (start < path.size()) {
        qsizetype end = path.indexOf('/', start);
        if (end < 0) {
            end = path.size();
        }
        const auto component = path.sliced(start, end - start);
        if (!isValidFileName(component)) {
            return std::nullopt;
        }
        if (result.share.isEmpty()) {
            result.share = component.toByteArray();
        } else {
            result.path += '\\';
            result.path += component;
        }
        start = end + 1;
    }

    if (result.path.isEmpty()) {
        result.path = QByteArrayLiteral("\\");
    }
    return result;
}

QByteArray parentPath(QByteArrayView path)
{
    const auto separator = path.lastIndexOf('\\');
    if (separator <= 0) {
        return QByteArrayLiteral("\\");
    }
    return path.first(separator).toByteArray();
}

QByteArray fileName(QByteArrayView path)
{
    return path.sliced(path.lastIndexOf('\\') + 1).toByteArray();
}

QString announcedDriveName(QByteArrayView preferredDosName, QByteArrayView deviceData)
{
    QString name;
    // A UTF-16 string ends with two zero bytes at an even offset, a string of
    // 8 bit characters with a single one.
    if (deviceData.size() >= 2 && deviceData.size() % 2 == 0 && deviceData.at(deviceData.size() - 1) == '\0' && deviceData.at(deviceData.size() - 2) == '\0') {
        QStringDecoder decoder(QStringDecoder::Utf16LE);
        name = decoder.decode(deviceData);
    } else {
        name = QString::fromLatin1(deviceData);
    }
    if (const auto end = name.indexOf(QChar::Null); end >= 0) {
        name.truncate(end);
    }

    if (name.trimmed().isEmpty()) {
        name = QString::fromLatin1(preferredDosName.first(std::min<qsizetype>(preferredDosName.size(), 8)));
        if (const auto end = name.indexOf(QChar::Null); end >= 0) {
            name.truncate(end);
        }
    }
    return name.trimmed();
}

QByteArray sanitizedShareName(const QString &name, quint32 deviceId)
{
    // Long enough for any drive name, and short enough to leave room for a
    // suffix that makes the name unique.
    static constexpr qsizetype MaximumLength = 64;

    QString sanitized;
    for (const QChar character : name.trimmed()) {
        if (sanitized.size() >= MaximumLength) {
            break;
        }
        if (character.isSurrogate() || !character.isPrint() || character == u'/' || character == u'\\') {
            sanitized += u'_';
        } else {
            sanitized += character;
        }
    }
    // Windows clients announce their drives as "C:".
    while (sanitized.endsWith(u':') || sanitized.endsWith(u' ')) {
        sanitized.chop(1);
    }

    QByteArray result = sanitized.toUtf8();
    // Hidden directories would only confuse, and the name must not be "..".
    if (result.startsWith('.') || !isValidFileName(result)) {
        result = QByteArrayLiteral("drive") + QByteArray::number(deviceId);
    }
    return result;
}

int ntStatusToErrno(quint32 status)
{
    switch (static_cast<NTSTATUS>(status)) {
    case STATUS_SUCCESS:
        return 0;
    case STATUS_NO_SUCH_FILE:
    case STATUS_NO_MORE_FILES:
    case STATUS_OBJECT_NAME_NOT_FOUND:
    case STATUS_OBJECT_PATH_NOT_FOUND:
    case STATUS_DELETE_PENDING:
    case STATUS_NOT_FOUND:
        return ENOENT;
    case STATUS_OBJECT_NAME_COLLISION:
        return EEXIST;
    case STATUS_ACCESS_DENIED:
    case STATUS_CANNOT_DELETE:
    case STATUS_PRIVILEGE_NOT_HELD:
        return EACCES;
    case STATUS_MEDIA_WRITE_PROTECTED:
        return EROFS;
    case STATUS_SHARING_VIOLATION:
    case STATUS_DEVICE_BUSY:
    case STATUS_USER_MAPPED_FILE:
        return EBUSY;
    case STATUS_FILE_LOCK_CONFLICT:
    case STATUS_LOCK_NOT_GRANTED:
        return EAGAIN;
    case STATUS_DISK_FULL:
        return ENOSPC;
    case STATUS_DIRECTORY_NOT_EMPTY:
        return ENOTEMPTY;
    case STATUS_NOT_A_DIRECTORY:
        return ENOTDIR;
    case STATUS_FILE_IS_A_DIRECTORY:
        return EISDIR;
    case STATUS_OBJECT_NAME_INVALID:
    case STATUS_OBJECT_PATH_INVALID:
    case STATUS_OBJECT_PATH_SYNTAX_BAD:
    case STATUS_INVALID_PARAMETER:
        return EINVAL;
    case STATUS_NAME_TOO_LONG:
        return ENAMETOOLONG;
    case STATUS_INVALID_HANDLE:
    case STATUS_FILE_CLOSED:
        return EBADF;
    case STATUS_NOT_SAME_DEVICE:
        return EXDEV;
    case STATUS_TOO_MANY_OPENED_FILES:
        return EMFILE;
    case STATUS_NO_MEMORY:
    case STATUS_INSUFFICIENT_RESOURCES:
        return ENOMEM;
    case STATUS_NOT_SUPPORTED:
    case STATUS_NOT_IMPLEMENTED:
    case STATUS_INVALID_DEVICE_REQUEST:
        return ENOTSUP;
    case STATUS_NO_SUCH_DEVICE:
    case STATUS_DEVICE_NOT_READY:
    case STATUS_DEVICE_DOES_NOT_EXIST:
        return ENODEV;
    case STATUS_IO_TIMEOUT:
    case STATUS_TIMEOUT:
        return ETIMEDOUT;
    case STATUS_CANCELLED:
        return ECANCELED;
    default:
        // Including warnings and informational values, nothing we request
        // is expected to return those.
        return EIO;
    }
}

timespec fileTimeToTimespec(qint64 fileTime)
{
    timespec result{};
    if (fileTime <= FileTimeEpochOffset) {
        return result;
    }
    const qint64 ticks = fileTime - FileTimeEpochOffset;
    result.tv_sec = ticks / FileTimeTicksPerSecond;
    result.tv_nsec = (ticks % FileTimeTicksPerSecond) * 100;
    return result;
}

qint64 timespecToFileTime(const timespec &time)
{
    return FileTimeEpochOffset + qint64(time.tv_sec) * FileTimeTicksPerSecond + time.tv_nsec / 100;
}

}
