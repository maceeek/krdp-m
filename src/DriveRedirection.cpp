// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DriveRedirection.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <QHash>
#include <QSet>

#include <freerdp/channels/rdpdr.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpdr.h>
#include <winpr/file.h>
#include <winpr/nt.h>

#include "DriveFilesystem.h"
#include "DriveUtils.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

// How long to wait for the client to answer a request, or to make progress
// on one that takes several answers.
static constexpr auto RequestTimeout = std::chrono::seconds(30);

namespace
{

/**
 * A request to the client that a thread of the file system is waiting for.
 */
struct Request {
    quint32 deviceId = 0;
    // Set instead of status when the request did not get an answer.
    int error = 0;
    quint32 status = quint32(STATUS_UNSUCCESSFUL);

    std::condition_variable condition;
    bool done = false;
    // The thread waiting for the request gave up on it.
    bool abandoned = false;
    // Incremented for each answer of requests that have several.
    quint64 progress = 0;

    quint32 fileId = 0;
    quint32 length = 0;
    char *buffer = nullptr;
    quint32 bufferSize = 0;
    QList<DriveEntry> entries;
    quint64 totalBytes = 0;
    quint64 availableBytes = 0;

    int result() const
    {
        return error != 0 ? error : -DriveUtils::ntStatusToErrno(status);
    }
};

struct Device {
    QString name;
    QByteArray shareName;
};

/**
 * The state of the channel, shared with the backends of the drives so that
 * they stay usable, if only to fail, for as long as the file system has
 * requests for them.
 */
class Channel : public std::enable_shared_from_this<Channel>
{
public:
    using Send = std::function<UINT(RdpdrServerContext *context, void *callbackData)>;

    DriveRedirection *q = nullptr;

    // Held shared while a request is sent, and exclusively to close the
    // channel. Not to be taken by the thread of the channel, which is joined
    // while it is held.
    std::shared_mutex contextMutex;
    RdpdrServerContext *context = nullptr;

    // Guards the requests and everything in them.
    std::mutex mutex;
    bool failed = false;
    quintptr nextRequestId = 1;
    std::unordered_map<quintptr, std::shared_ptr<Request>> requests;
    QSet<quint32> removedDevices;

    // Guards the devices.
    std::mutex devicesMutex;
    bool closed = false;
    // Drives the client announced that are not available as a share yet.
    QList<std::pair<quint32, QString>> announcedDevices;
    QHash<quint32, Device> devices;

    std::shared_ptr<Request> perform(quint32 deviceId, const Send &send, char *buffer = nullptr, quint32 bufferSize = 0);
    // Find the request an answer is for. Has to be called with the mutex held.
    std::shared_ptr<Request> find(void *callbackData, bool remove = true);
    void complete(void *callbackData, quint32 status);
    void fail(std::optional<quint32> deviceId, int error);

    void addAnnouncedDevices();
    void removeDevice(quint32 deviceId);
    void removeAllDevices();
};

std::shared_ptr<Request> Channel::perform(quint32 deviceId, const Send &send, char *buffer, quint32 bufferSize)
{
    auto request = std::make_shared<Request>();
    request->deviceId = deviceId;
    request->buffer = buffer;
    request->bufferSize = bufferSize;

    quintptr id = 0;
    {
        std::lock_guard lock(mutex);
        if (failed) {
            request->error = -EIO;
            return request;
        }
        if (removedDevices.contains(deviceId)) {
            request->error = -ENODEV;
            return request;
        }
        id = nextRequestId++;
        requests.emplace(id, request);
    }

    {
        std::shared_lock contextLock(contextMutex);
        const UINT result = context ? send(context, reinterpret_cast<void *>(id)) : ERROR_INTERNAL_ERROR;
        if (result != CHANNEL_RC_OK) {
            std::lock_guard lock(mutex);
            requests.erase(id);
            request->error = -EIO;
            return request;
        }
    }

    std::unique_lock lock(mutex);
    quint64 seenProgress = request->progress;
    while (!request->done) {
        if (request->condition.wait_for(lock, RequestTimeout) == std::cv_status::timeout && !request->done) {
            if (request->progress != seenProgress) {
                seenProgress = request->progress;
                continue;
            }
            qCWarning(KRDP) << "The client did not answer a request for redirected drive" << deviceId;
            // Stays around in case the answer arrives after all.
            request->abandoned = true;
            request->buffer = nullptr;
            request->error = -ETIMEDOUT;
            break;
        }
    }
    return request;
}

std::shared_ptr<Request> Channel::find(void *callbackData, bool remove)
{
    const auto it = requests.find(reinterpret_cast<quintptr>(callbackData));
    if (it == requests.end()) {
        return nullptr;
    }
    auto request = it->second;
    if (remove) {
        requests.erase(it);
    }
    return request;
}

void Channel::complete(void *callbackData, quint32 status)
{
    std::lock_guard lock(mutex);
    if (auto request = find(callbackData)) {
        request->status = status;
        request->done = true;
        request->condition.notify_all();
    }
}

void Channel::fail(std::optional<quint32> deviceId, int error)
{
    std::lock_guard lock(mutex);
    if (deviceId) {
        removedDevices.insert(*deviceId);
    } else {
        failed = true;
    }
    std::erase_if(requests, [&](const auto &item) {
        const auto &request = item.second;
        if (deviceId && request->deviceId != *deviceId) {
            return false;
        }
        request->error = error;
        request->buffer = nullptr;
        request->done = true;
        request->condition.notify_all();
        return true;
    });
}

/**
 * A redirected drive of the client as the file system sees it.
 */
class RdpDrive : public DriveBackend
{
public:
    RdpDrive(const std::shared_ptr<Channel> &channel, quint32 deviceId)
        : m_channel(channel)
        , m_deviceId(deviceId)
    {
    }

    int list(const QByteArray &path, QList<DriveEntry> &entries) override
    {
        // FreeRDP appends a separator and a wildcard to the path to list. The
        // root is the empty path here, so that it does not end up with two
        // separators, which Windows does not accept.
        const QByteArray directory = path == "\\" ? QByteArrayLiteral("") : path;
        const auto request = m_channel->perform(m_deviceId, [&](RdpdrServerContext *context, void *data) {
            return context->DriveQueryDirectory(context, data, m_deviceId, directory.constData());
        });
        // Windows clients say this for the root of a drive without any
        // files, the only directory that does not at least contain "." and
        // "..". For other directories it means that they do not exist.
        if (request->error == 0 && request->status == quint32(STATUS_NO_SUCH_FILE) && path == "\\") {
            return 0;
        }
        if (const int result = request->result(); result != 0) {
            return result;
        }
        entries = std::move(request->entries);
        return 0;
    }

    int open(const QByteArray &path, Access access, Disposition disposition, quint32 &fileId) override
    {
        // Specific rights, clients do not map the generic ones.
        UINT32 desiredAccess = 0;
        switch (access) {
        case Access::Read:
            desiredAccess = FILE_GENERIC_READ;
            break;
        case Access::Write:
            desiredAccess = FILE_GENERIC_WRITE;
            break;
        case Access::ReadWrite:
            desiredAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
            break;
        }

        UINT32 createDisposition = FILE_OPEN;
        switch (disposition) {
        case Disposition::Open:
            createDisposition = FILE_OPEN;
            break;
        case Disposition::Truncate:
            createDisposition = FILE_OVERWRITE;
            break;
        case Disposition::Create:
            createDisposition = FILE_CREATE;
            break;
        case Disposition::OpenOrCreate:
            createDisposition = FILE_OPEN_IF;
            break;
        case Disposition::CreateOrTruncate:
            createDisposition = FILE_OVERWRITE_IF;
            break;
        }

        const auto request = m_channel->perform(m_deviceId, [&](RdpdrServerContext *context, void *data) {
            return context->DriveOpenFile(context, data, m_deviceId, path.constData(), desiredAccess, createDisposition);
        });
        fileId = request->fileId;
        return request->result();
    }

    int close(quint32 fileId) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveCloseFile(context, data, m_deviceId, fileId);
                      })
            ->result();
    }

    int read(quint32 fileId, char *data, quint32 size, quint64 offset) override
    {
        const auto request = m_channel->perform(
            m_deviceId,
            [&](RdpdrServerContext *context, void *callbackData) {
                return context->DriveReadFileEx(context, callbackData, m_deviceId, fileId, size, offset);
            },
            data,
            size);
        if (request->error == 0 && request->status == quint32(STATUS_END_OF_FILE)) {
            return 0;
        }
        const int result = request->result();
        return result != 0 ? result : int(request->length);
    }

    int write(quint32 fileId, const char *data, quint32 size, quint64 offset) override
    {
        const auto request = m_channel->perform(m_deviceId, [&](RdpdrServerContext *context, void *callbackData) {
            return context->DriveWriteFileEx(context, callbackData, m_deviceId, fileId, data, size, offset);
        });
        const int result = request->result();
        return result != 0 ? result : int(std::min(request->length, size));
    }

    int createDirectory(const QByteArray &path) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveCreateDirectory(context, data, m_deviceId, path.constData());
                      })
            ->result();
    }

    int removeDirectory(const QByteArray &path) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveDeleteDirectory(context, data, m_deviceId, path.constData());
                      })
            ->result();
    }

    int removeFile(const QByteArray &path) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveDeleteFile(context, data, m_deviceId, path.constData());
                      })
            ->result();
    }

    int rename(const QByteArray &from, const QByteArray &to, bool replace) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveRenameFileEx(context, data, m_deviceId, from.constData(), to.constData(), replace);
                      })
            ->result();
    }

    int resize(const QByteArray &path, quint64 size) override
    {
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveSetFileSize(context, data, m_deviceId, path.constData(), size);
                      })
            ->result();
    }

    int setTimes(const QByteArray &path, const timespec *accessTime, const timespec *modificationTime) override
    {
        // A time of 0 is left as it is.
        const INT64 lastAccessTime = accessTime ? DriveUtils::timespecToFileTime(*accessTime) : 0;
        const INT64 lastWriteTime = modificationTime ? DriveUtils::timespecToFileTime(*modificationTime) : 0;
        return m_channel
            ->perform(m_deviceId,
                      [&](RdpdrServerContext *context, void *data) {
                          return context->DriveSetFileTimes(context, data, m_deviceId, path.constData(), lastAccessTime, lastWriteTime);
                      })
            ->result();
    }

    int freeSpace(quint64 &totalBytes, quint64 &availableBytes) override
    {
        const auto request = m_channel->perform(m_deviceId, [&](RdpdrServerContext *context, void *data) {
            return context->DriveQueryVolumeInformation(context, data, m_deviceId);
        });
        totalBytes = request->totalBytes;
        availableBytes = request->availableBytes;
        return request->result();
    }

private:
    const std::shared_ptr<Channel> m_channel;
    const quint32 m_deviceId;
};

void Channel::addAnnouncedDevices()
{
    std::lock_guard lock(devicesMutex);
    const auto announced = std::exchange(announcedDevices, {});
    if (closed) {
        return;
    }

    for (const auto &[deviceId, name] : announced) {
        if (devices.contains(deviceId)) {
            continue;
        }
        {
            // The ID may have been used by a drive that was removed.
            std::lock_guard requestsLock(mutex);
            removedDevices.remove(deviceId);
        }

        auto filesystem = DriveFilesystem::shared();
        const auto shareName = filesystem->addShare(DriveUtils::sanitizedShareName(name, deviceId), std::make_shared<RdpDrive>(shared_from_this(), deviceId));
        if (shareName.isEmpty()) {
            qCWarning(KRDP) << "Could not make redirected drive" << name << "available";
            continue;
        }

        const QString path = filesystem->mountPoint() + u'/' + QString::fromUtf8(shareName);
        qCInfo(KRDP) << "Redirected drive" << name << "is available at" << path;
        devices.insert(deviceId, Device{.name = name, .shareName = shareName});
        Q_EMIT q->driveAdded(name, path);
    }
}

void Channel::removeDevice(quint32 deviceId)
{
    // Make the threads of the file system that wait for the drive return,
    // removing the share waits for them.
    fail(deviceId, -ENODEV);

    Device device;
    {
        std::lock_guard lock(devicesMutex);
        announcedDevices.removeIf([deviceId](const auto &announced) {
            return announced.first == deviceId;
        });
        const auto it = devices.constFind(deviceId);
        if (it == devices.constEnd()) {
            return;
        }
        device = *it;
        devices.erase(it);
    }

    DriveFilesystem::shared()->removeShare(device.shareName);
    qCInfo(KRDP) << "Redirected drive" << device.name << "was removed";
    Q_EMIT q->driveRemoved(device.name);
}

void Channel::removeAllDevices()
{
    fail(std::nullopt, -EIO);

    QHash<quint32, Device> removed;
    {
        std::lock_guard lock(devicesMutex);
        closed = true;
        announcedDevices.clear();
        removed = std::exchange(devices, {});
    }

    for (const auto &device : std::as_const(removed)) {
        DriveFilesystem::shared()->removeShare(device.shareName);
        qCInfo(KRDP) << "Redirected drive" << device.name << "was removed";
        Q_EMIT q->driveRemoved(device.name);
    }
}

Channel *channelOf(RdpdrServerContext *context)
{
    return static_cast<Channel *>(context->data);
}

// Callbacks of FreeRDP, called from the thread of the channel.

UINT onDriveCreate(RdpdrServerContext *context, const RdpdrDevice *device)
{
    auto channel = channelOf(context);
    const auto name = DriveUtils::announcedDriveName(QByteArrayView(device->PreferredDosName, qstrnlen(device->PreferredDosName, 8)),
                                                     QByteArrayView(reinterpret_cast<const char *>(device->DeviceData), device->DeviceDataLength));
    qCDebug(KRDP) << "Client announced drive" << name << "with ID" << device->DeviceId;

    // The share is added once the client was told that we accept the drive.
    std::lock_guard lock(channel->devicesMutex);
    channel->announcedDevices.append({device->DeviceId, name});
    return CHANNEL_RC_OK;
}

UINT onDriveDelete(RdpdrServerContext *context, UINT32 deviceId)
{
    channelOf(context)->removeDevice(deviceId);
    return CHANNEL_RC_OK;
}

UINT onReceivePdu(RdpdrServerContext *context, const RDPDR_HEADER *header, UINT error)
{
    auto channel = channelOf(context);
    if (error != CHANNEL_RC_OK) {
        // The thread of the channel stops after this, nothing is going to be
        // answered anymore.
        qCWarning(KRDP) << "Drive redirection failed with error" << error << "while handling" << rdpdr_packetid_string(header->PacketId);
        channel->removeAllDevices();
    } else if (header->Component == RDPDR_CTYP_CORE && header->PacketId == PAKID_CORE_DEVICELIST_ANNOUNCE) {
        channel->addAnnouncedDevices();
    }
    return error;
}

void onStatusComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus)
{
    channelOf(context)->complete(callbackData, ioStatus);
}

void onQueryDirectoryComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus, FILE_DIRECTORY_INFORMATION *information)
{
    auto channel = channelOf(context);
    std::lock_guard lock(channel->mutex);

    if (ioStatus != quint32(STATUS_SUCCESS)) {
        // The end of the listing, or the reason there is none.
        if (auto request = channel->find(callbackData)) {
            request->status = ioStatus == quint32(STATUS_NO_MORE_FILES) ? quint32(STATUS_SUCCESS) : ioStatus;
            request->done = true;
            request->condition.notify_all();
        }
        return;
    }

    auto request = channel->find(callbackData, false);
    if (!request || request->abandoned) {
        return;
    }
    ++request->progress;
    if (!information) {
        return;
    }

    DriveEntry entry;
    entry.name = QByteArray(information->FileName, qstrnlen(information->FileName, sizeof(information->FileName)));
    entry.directory = (information->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.readOnly = (information->FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    entry.size = quint64(std::max<LONGLONG>(information->EndOfFile.QuadPart, 0));
    entry.accessTime = DriveUtils::fileTimeToTimespec(information->LastAccessTime.QuadPart);
    entry.modificationTime = DriveUtils::fileTimeToTimespec(information->LastWriteTime.QuadPart);
    entry.changeTime = information->ChangeTime.QuadPart > 0 ? DriveUtils::fileTimeToTimespec(information->ChangeTime.QuadPart) : entry.modificationTime;
    request->entries.append(std::move(entry));
}

void onOpenFileComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus, UINT32 deviceId, UINT32 fileId)
{
    auto channel = channelOf(context);
    {
        std::lock_guard lock(channel->mutex);
        auto request = channel->find(callbackData);
        if (request && !request->abandoned) {
            request->status = ioStatus;
            request->fileId = fileId;
            request->done = true;
            request->condition.notify_all();
            return;
        }
    }

    // Nobody is waiting for the file anymore, do not leave it open. The
    // answer is for a request that does not exist and is ignored.
    if (ioStatus == quint32(STATUS_SUCCESS)) {
        const UINT result = context->DriveCloseFile(context, reinterpret_cast<void *>(std::numeric_limits<quintptr>::max()), deviceId, fileId);
        if (result != CHANNEL_RC_OK) {
            qCDebug(KRDP) << "Could not close a file of redirected drive" << deviceId;
        }
    }
}

void onReadFileComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus, const char *buffer, UINT32 length)
{
    auto channel = channelOf(context);
    std::lock_guard lock(channel->mutex);
    if (auto request = channel->find(callbackData)) {
        request->status = ioStatus;
        if (ioStatus == quint32(STATUS_SUCCESS) && request->buffer && buffer) {
            // Never more than was asked for, whatever the client sends.
            request->length = std::min(length, request->bufferSize);
            memcpy(request->buffer, buffer, request->length);
        }
        request->done = true;
        request->condition.notify_all();
    }
}

void onWriteFileComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus, UINT32 bytesWritten)
{
    auto channel = channelOf(context);
    std::lock_guard lock(channel->mutex);
    if (auto request = channel->find(callbackData)) {
        request->status = ioStatus;
        request->length = bytesWritten;
        request->done = true;
        request->condition.notify_all();
    }
}

void onQueryVolumeInformationComplete(RdpdrServerContext *context, void *callbackData, UINT32 ioStatus, UINT64 totalBytes, UINT64 availableBytes)
{
    auto channel = channelOf(context);
    std::lock_guard lock(channel->mutex);
    if (auto request = channel->find(callbackData)) {
        request->status = ioStatus;
        request->totalBytes = totalBytes;
        request->availableBytes = availableBytes;
        request->done = true;
        request->condition.notify_all();
    }
}

}

class KRDP_NO_EXPORT DriveRedirection::Private
{
public:
    RdpConnection *session = nullptr;
    std::shared_ptr<Channel> channel;
    // Only try once, a channel that could not be opened will not work later either.
    bool initialized = false;
    bool started = false;
};

DriveRedirection::DriveRedirection(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
    d->channel = std::make_shared<Channel>();
    d->channel->q = this;
}

DriveRedirection::~DriveRedirection()
{
    close();
}

bool DriveRedirection::initialize()
{
    if (d->initialized) {
        return d->started;
    }
    d->initialized = true;

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeer()->context);

    auto context = rdpdr_server_context_new(peerContext->virtualChannelManager);
    if (!context) {
        qCWarning(KRDP) << "Failed creating drive redirection context";
        return false;
    }

    context->rdpcontext = d->session->rdpPeer()->context;
    context->data = d->channel.get();
    // Everything else is refused when the client announces it.
    context->supported = RDPDR_DTYP_FILESYSTEM;

    context->OnDriveCreate = onDriveCreate;
    context->OnDriveDelete = onDriveDelete;
    context->ReceivePDU = onReceivePdu;
    context->OnDriveCreateDirectoryComplete = onStatusComplete;
    context->OnDriveDeleteDirectoryComplete = onStatusComplete;
    context->OnDriveQueryDirectoryComplete = onQueryDirectoryComplete;
    context->OnDriveOpenFileComplete = onOpenFileComplete;
    context->OnDriveReadFileComplete = onReadFileComplete;
    context->OnDriveWriteFileComplete = onWriteFileComplete;
    context->OnDriveCloseFileComplete = onStatusComplete;
    context->OnDriveDeleteFileComplete = onStatusComplete;
    context->OnDriveRenameFileComplete = onStatusComplete;
    context->OnDriveSetFileInformationComplete = onStatusComplete;
    context->OnDriveQueryVolumeInformationComplete = onQueryVolumeInformationComplete;

    {
        std::unique_lock lock(d->channel->contextMutex);
        d->channel->context = context;
    }

    if (context->Start(context) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not start drive redirection context";
        std::unique_lock lock(d->channel->contextMutex);
        d->channel->context = nullptr;
        rdpdr_server_context_free(context);
        return false;
    }

    d->started = true;
    qCDebug(KRDP) << "Drive redirection channel initialized";
    return true;
}

void DriveRedirection::close()
{
    // Fails the requests the file system is waiting for and removes the
    // shares, after which nothing calls into the channel anymore.
    d->channel->removeAllDevices();

    std::unique_lock lock(d->channel->contextMutex);
    auto context = std::exchange(d->channel->context, nullptr);
    if (!context) {
        return;
    }
    if (d->started && context->Stop(context) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Could not stop drive redirection context";
    }
    d->started = false;
    rdpdr_server_context_free(context);
}

}

#include "moc_DriveRedirection.cpp"
