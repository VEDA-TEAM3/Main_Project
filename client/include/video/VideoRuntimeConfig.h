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
    qint64 historyMsec = 0;
    qint64 matchToleranceMsec = 0;
    qint64 holdLastMetadataMsec = 0;
    // 다음 metadata가 아직 오지 않은 구간에서 마지막 두 프레임의 이동량으로 위치를 예측하는
    // 최대 시간. 0이면 예측하지 않고 마지막 위치를 그대로 유지한다
    qint64 maximumExtrapolationMsec = 0;
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
    // queue는 전부 시간으로만 제한한다. buffer 개수 상한은 같은 시간이라도 fps에 따라 값이 달라져서,
    // 30fps에서 정렬 지연보다 먼저 걸리면 지연선이 조용히 무너진다
    qint64 decodeQueueMaximumTimeMsec = 0;
    // Intentional decoded-video playout delay used to align video with slower AI/MQTT state updates.
    // sink의 ts-offset으로 적용하므로 sinkSync=true가 전제다(설정 로더가 검증한다).
    qint64 alignmentDelayMsec = 250;
    // 정렬 지연 위에 얹는 순수 여유분. renderqueue의 실제 상한은 이 값 + alignmentDelayMsec다
    qint64 renderQueueMaximumTimeMsec = 0;
    bool sinkQos = false;
    bool sinkSync = true;
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
