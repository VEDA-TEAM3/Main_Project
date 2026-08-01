#pragma once

#include <QMetaType>
#include <QVector>
#include <QtGlobal>

struct VideoFrameTimestamp {
    int channelIndex = -1;
    qint64 utcMsec = 0;
    qint64 observedLocalMsec = 0;
    bool senderClock = false;

    bool isValid() const { return channelIndex >= 0 && utcMsec > 0 && observedLocalMsec > 0; }
};

Q_DECLARE_METATYPE(VideoFrameTimestamp)
Q_DECLARE_METATYPE(QVector<VideoFrameTimestamp>)
