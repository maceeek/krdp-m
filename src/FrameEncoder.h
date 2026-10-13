// SPDX-FileCopyrightText: 2026 KRdp-M contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>

#include "krdp_export.h"

namespace KRdp
{

/**
 * Encodes raw frames from a PipeWireSourceStream on a thread of its own, for
 * the encoders KPipeWire does not have.
 *
 * When frames come in faster than they can be encoded or sent only the latest
 * one is kept. Subclasses encode a frame in encodeImage().
 */
class KRDP_EXPORT FrameEncoder : public QObject
{
    Q_OBJECT

public:
    ~FrameEncoder() override;

    /**
     * Set the quality from 0 to 100. When it changes, the next frame is a
     * key frame.
     */
    void setQuality(quint8 quality);

    /**
     * Encode \p image, replacing a frame that is still waiting to be encoded.
     */
    void encode(const QImage &image);

    /**
     * Encode the next frame as a key frame. Without a new frame the last
     * frame is encoded again.
     */
    void requestKeyFrame();

    /**
     * A frame of \p size was encoded. Emitted from the encoding thread.
     *
     * Keep \p sent until the packet was sent to the client or dropped. Until
     * then no other frame is encoded, so frames don't queue up.
     */
    Q_SIGNAL void packetReady(const QByteArray &data, bool isKeyFrame, const QSize &size, const std::shared_ptr<void> &sent);
    /**
     * The encoder could not be set up or stopped working, nothing will be
     * encoded anymore. Emitted from the encoding thread.
     */
    Q_SIGNAL void failed();

protected:
    FrameEncoder();

    /**
     * Start the encoding thread, at the end of the constructor of the subclass.
     */
    void start();
    /**
     * Stop the encoding thread, at the start of the destructor of the
     * subclass, while encodeImage() can still be called.
     */
    void stop();

    quint8 quality() const;

    /**
     * Encode \p image on the encoding thread, calling emitPacket() for the
     * packets. Returns false if the encoder does not work.
     */
    virtual bool encodeImage(const QImage &image, bool keyFrame) = 0;
    /**
     * Free the encoder, once it failed.
     */
    virtual void close() = 0;
    void emitPacket(const QByteArray &data, bool isKeyFrame, const QSize &size);

private:
    class Private;
    const std::unique_ptr<Private> d;
};

}
