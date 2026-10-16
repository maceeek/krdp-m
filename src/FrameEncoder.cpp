// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "FrameEncoder.h"
#include "krdp_logging.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace KRdp
{

// Packets that were encoded but not sent yet. Encoding more than the client
// takes would only queue them up and add latency, as video frames can't be
// dropped. Instead, newer frames replace older ones before encoding.
constexpr int MaximumUnsentPackets = 1;

class KRDP_NO_EXPORT FrameEncoder::Private
{
public:
    void run(std::stop_token token, FrameEncoder *q);

    std::atomic<quint8> quality = 75;
    std::atomic_uint64_t replacedFrames = 0;
    quint64 encodedFrames = 0;
    std::chrono::steady_clock::duration encodingTime{};
    std::chrono::steady_clock::time_point lastStatistics = std::chrono::steady_clock::now();

    /**
     * Shared with the tokens of packets, which may outlive the encoder.
     */
    struct Input {
        std::mutex mutex;
        std::condition_variable_any condition;
        // Guarded by mutex.
        std::optional<QImage> pending;
        bool keyFrameRequested = false;
        int unsentPackets = 0;
    };
    const std::shared_ptr<Input> input = std::make_shared<Input>();

    // Only used on the encoding thread.
    QImage lastImage;

    std::jthread thread;
};

FrameEncoder::FrameEncoder()
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
}

FrameEncoder::~FrameEncoder()
{
    Q_ASSERT(!d->thread.joinable());
}

void FrameEncoder::start()
{
    d->thread = std::jthread([this](std::stop_token token) {
        d->run(token, this);
    });
}

void FrameEncoder::stop()
{
    if (d->thread.joinable()) {
        d->thread.request_stop();
        d->thread.join();
    }
}

quint8 FrameEncoder::quality() const
{
    return d->quality;
}

void FrameEncoder::setQuality(quint8 quality)
{
    d->quality = quality;
}

void FrameEncoder::encode(const QImage &image)
{
    {
        std::lock_guard lock(d->input->mutex);
        if (d->input->pending) {
            ++d->replacedFrames;
        }
        d->input->pending = image;
    }
    d->input->condition.notify_one();
}

void FrameEncoder::requestKeyFrame()
{
    {
        std::lock_guard lock(d->input->mutex);
        d->input->keyFrameRequested = true;
    }
    d->input->condition.notify_one();
}

void FrameEncoder::emitPacket(const QByteArray &data, bool isKeyFrame, const QSize &size)
{
    {
        std::lock_guard lock(d->input->mutex);
        ++d->input->unsentPackets;
    }
    // Released once the packet was sent or dropped.
    const auto sent = std::shared_ptr<void>(nullptr, [input = d->input](void *) {
        {
            std::lock_guard lock(input->mutex);
            --input->unsentPackets;
        }
        input->condition.notify_one();
    });
    Q_EMIT packetReady(data, isKeyFrame, size, sent);
}

void FrameEncoder::Private::run(std::stop_token token, FrameEncoder *q)
{
    while (!token.stop_requested()) {
        QImage image;
        bool keyFrame = false;
        {
            std::unique_lock lock(input->mutex);
            if (!input->condition.wait(lock, token, [this] {
                    return (input->pending.has_value() || input->keyFrameRequested) && input->unsentPackets < MaximumUnsentPackets;
                })) {
                return;
            }
            if (input->pending) {
                image = std::move(*input->pending);
                input->pending.reset();
            } else {
                image = lastImage;
            }
            keyFrame = std::exchange(input->keyFrameRequested, false);
        }

        // A key frame requested before the first frame, the first frame
        // will be one.
        if (image.isNull()) {
            continue;
        }

        lastImage = image;
        const auto encodeStart = std::chrono::steady_clock::now();
        if (!q->encodeImage(image, keyFrame)) {
            q->close();
            Q_EMIT q->failed();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        encodingTime += now - encodeStart;
        ++encodedFrames;
        if (now - lastStatistics >= std::chrono::seconds(5)) {
            qCInfo(KRDP) << "Encoder stats:" << encodedFrames << "frames, mean encode ms"
                         << std::chrono::duration<double, std::milli>(encodingTime).count() / encodedFrames << "raw frames replaced"
                         << replacedFrames.exchange(0);
            lastStatistics = now;
            encodedFrames = 0;
            encodingTime = {};
        }
    }
}

}

#include "moc_FrameEncoder.cpp"
