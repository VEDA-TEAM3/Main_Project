#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include "model/StreamConfig.h"
#include "model/VideoPreprocessingSettings.h"

inline constexpr int videoChannelsPerArea = 4;

constexpr int videoGlobalChannelIndex(int areaIndex, int localChannelIndex) noexcept {
    return areaIndex * videoChannelsPerArea + localChannelIndex;
}

constexpr int videoLocalChannelIndex(int globalChannelIndex) noexcept {
    return globalChannelIndex % videoChannelsPerArea;
}

constexpr int videoLocalChannelNumber(int globalChannelIndex) noexcept {
    return videoLocalChannelIndex(globalChannelIndex) + 1;
}

static_assert(videoGlobalChannelIndex(1, 3) == 7);
static_assert(videoLocalChannelIndex(7) == 3);

struct VideoAreaConfig {
    QString areaId;
    QString name;
    QVector<int> channelIndexes;
};

struct BlurProcessorConfig {
    qint64 syncOffsetMsec = 0;
    // 영상 쪽 alignmentDelayMs와 짝을 이루는 블러 전용 수동 보정값. syncOffsetMs가 RTCP sender
    // clock이 없을 때만 걸리는 것과 달리 이 값은 항상 더해진다. 양수면 더 과거의 metadata를,
    // 음수면 더 최근의 metadata를 프레임에 맞춘다. 0이면 아무 영향이 없다.
    qint64 alignmentOffsetMsec = 0;
    qint64 historyMsec = 0;
    qint64 matchToleranceMsec = 0;
    qint64 holdLastMetadataMsec = 0;
    qsizetype maximumHistorySize = 0;
    qint64 sourceRestartGapMsec = 0;
    double paddingRatio = 0.0;
    int radiusDivisor = 0;
    int minimumRadius = 0;
    int maximumRadius = 0;
    int debugLogIntervalMsec = 0;
};

struct GstRtspReceiverConfig {
    QString decoderMode;
    // 시스템 메모리로 내려받기 전에 GPU에서 줄일 해상도. 블러·전처리가 CPU에서 도는 구간의
    // 픽셀 수와 GPU↔CPU 전송량을 함께 줄인다. 0이면 원본 해상도를 유지한다(d3d11 경로 전용).
    int processingWidth = 0;
    int processingHeight = 0;
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
    // Intentional decoded-video playout delay used to align video with slower AI/MQTT state updates.
    qint64 alignmentDelayMsec = 250;
    qint64 alignmentQueueMaximumTimeMsec = 450;
    int renderQueueMaximumBuffers = 0;
    qint64 renderQueueMaximumTimeMsec = 0;
    bool sinkQos = false;
    bool sinkSync = false;
    bool sinkAsync = false;
    qint64 minimumLoadingMsec = 0;
    int reconnectSpreadMsec = 0;
    VideoPreprocessingSettings preprocessing;
    BlurProcessorConfig blur;
};

struct VideoRuntimeConfig {
    QVector<StreamConfig> streams;
    QVector<VideoAreaConfig> areas;
    int initialAreaIndex = 0;
    int initialStartDelayMsec = 0;
    int receiverStartSpacingMsec = 0;
    GstRtspReceiverConfig receiver;
};
