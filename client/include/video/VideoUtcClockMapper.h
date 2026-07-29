#pragma once

#include <gst/gst.h>

#include <QMutex>
#include <QtGlobal>
#include <optional>

struct VideoUtcTimestamp {
    qint64 utcMsec = 0;
    bool senderClock = false;
};

class VideoUtcClockMapper final {
public:
    void reset();
    void observe(const GstBuffer* buffer);
    std::optional<VideoUtcTimestamp> timestampFor(const GstBuffer* buffer);

private:
    static std::optional<qint64> referenceUtcMsec(const GstBuffer* buffer);
    static std::optional<qint64> ptsMsec(const GstBuffer* buffer);
    void updateAnchor(qint64 ptsMsec, qint64 utcMsec, bool senderClock);

    QMutex mutex_;
    qint64 anchorPtsMsec_ = 0;
    qint64 anchorUtcMsec_ = 0;
    bool anchorReady_ = false;
    bool senderClockReady_ = false;
};
