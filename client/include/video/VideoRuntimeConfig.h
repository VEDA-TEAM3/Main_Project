#pragma once

#include <QVector>
#include <QtGlobal>

#include "model/StreamConfig.h"

struct BlurProcessorConfig {
    qint64 syncOffsetMsec = 0;
    qint64 historyMsec = 0;
    qint64 matchToleranceMsec = 0;
    qint64 holdLastMetadataMsec = 0;
    qsizetype maximumHistorySize = 0;
    qint64 sourceRestartGapMsec = 0;
    qint64 sourceTimestampRestartThresholdMsec = 0;
    double paddingRatio = 0.0;
    int maximumCornerRadius = 0;
    int radiusDivisor = 0;
    int minimumRadius = 0;
    int maximumRadius = 0;
    int debugLogIntervalMsec = 0;
};

struct GstRtspReceiverConfig {
    QString decoderMode;
    int busPollIntervalMsec = 0;
    int latencyMsec = 0;
    bool dropOnLatency = true;
    int initialPacketTimeoutMsec = 0;
    int initialFrameTimeoutMsec = 0;
    int maximumReconnectDelayMsec = 0;
    int authenticationFailureReconnectDelayMsec = 0;
    int stallTimeoutMsec = 0;
    quint64 udpBufferSizeBytes = 0;
    quint64 tcpTimeoutUsec = 0;
    quint64 udpTimeoutUsec = 0;
    int probationPackets = 0;
    bool rtspKeepAlive = true;
    bool udpReconnect = true;
    bool addReferenceTimestampMeta = true;
    int decodeQueueMaximumBuffers = 0;
    qint64 decodeQueueMaximumTimeMsec = 0;
    int renderQueueMaximumBuffers = 0;
    qint64 renderQueueMaximumTimeMsec = 0;
    bool sinkQos = false;
    bool sinkSync = false;
    bool sinkAsync = false;
    qint64 minimumLoadingMsec = 0;
    int reconnectSpreadMsec = 0;
    BlurProcessorConfig blur;
};

struct VideoRuntimeConfig {
    QVector<StreamConfig> streams;
    int initialStartDelayMsec = 0;
    int receiverStartSpacingMsec = 0;
    GstRtspReceiverConfig receiver;
};
