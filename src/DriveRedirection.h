// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QObject>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

/**
 * Implementation of the server side of drive redirection, part of the File
 * System Virtual Channel Extension ([MS-RDPEFS], the `rdpdr` channel).
 *
 * Clients use this channel to share folders with the session, for example
 * with `/drive:name,/path` of FreeRDP or the "Share Media" setting of KRDC.
 * Each shared folder shows up as a directory of the file system
 * DriveFilesystem mounts in the home directory, and goes away again when the
 * client removes it or disconnects.
 *
 * Other kinds of redirected devices, like printers and smart cards, are
 * refused.
 */
class KRDP_EXPORT DriveRedirection : public QObject
{
    Q_OBJECT
public:
    explicit DriveRedirection(RdpConnection *session);
    ~DriveRedirection() override;

    /**
     * Open the channel and wait for the client to announce its drives.
     *
     * Failure is not fatal for the connection, it only means the client
     * cannot share folders.
     */
    bool initialize();
    /**
     * Remove the drives of the client and close the channel.
     *
     * Requests that are in progress fail, as do those of programs that still
     * have files of the drives open.
     */
    void close();

Q_SIGNALS:
    /**
     * Emitted when a drive of the client is available.
     *
     * \param name The name of the drive.
     * \param path The directory the drive can be found in.
     *
     * \note This is emitted from the thread of the channel.
     */
    void driveAdded(const QString &name, const QString &path);
    /**
     * Emitted when a drive of the client is no longer available.
     *
     * \note This is emitted from the thread of the channel, or the thread
     *       calling close().
     */
    void driveRemoved(const QString &name);

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
