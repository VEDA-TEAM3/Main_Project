#include "network/BlurFrameDispatcher.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <utility>

#include "network/BlurFrameBuffer.h"

namespace {
constexpr int statisticsLogIntervalMsec = 5000;
}

/**
 * @brief         채널별 블러 프레임을 짧게 모아 순서대로 전달하는 dispatcher를 생성합니다.
 * @param config  JSON 검증을 통과한 MQTT dispatcher 설정
 * @param parent  Qt 객체 소유권을 연결할 부모 객체
 */
BlurFrameDispatcher::BlurFrameDispatcher(MqttDispatcherConfig config, std::shared_ptr<BlurFrameBuffer> frameBuffer,
                                         QObject* parent)
    : QObject(parent), frameBuffer_(std::move(frameBuffer)), config_(std::move(config)) {
    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(config_.blurFlushIntervalMsec);
    flushTimer_->setSingleShot(true);
    flushTimer_->setTimerType(Qt::PreciseTimer);
    connect(flushTimer_, &QTimer::timeout, this, &BlurFrameDispatcher::flushPendingFrames);
}

/**
 * @brief 블러 프레임 병합을 시작합니다.
 */
void BlurFrameDispatcher::start() { running_ = true; }

/**
 * @brief 예약된 블러 프레임과 timestamp 이력을 정리합니다.
 */
void BlurFrameDispatcher::stop() {
    running_ = false;
    flushTimer_->stop();
    if (frameBuffer_) {
        frameBuffer_->clear();
    }
    latestSourceTimes_.clear();
    lastArrivalTimes_.clear();
    lastStatisticsLogMsec_ = 0;
    deliveredFrameCount_ = 0;
}

/**
 * @brief        채널의 최신 블러 프레임을 저장하고 전달을 예약합니다.
 * @param frame  파싱과 채널 검증을 마친 블러 프레임
 */
void BlurFrameDispatcher::submitFrame(BlurFrameData frame) {
    if (!running_ || !frameBuffer_ || frame.channelIndex < 0 || frame.sourceTimestamp <= 0) {
        return;
    }

    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    const int channelIndex = frame.channelIndex;
    const qint64 lastArrivalMsec = lastArrivalTimes_.value(channelIndex, 0);
    qint64 latestSourceTimestamp = latestSourceTimes_.value(channelIndex, 0);
    const bool arrivalRestart = lastArrivalMsec > 0 && nowMsec - lastArrivalMsec > config_.blurSourceRestartGapMsec;
    const bool timestampRestart =
        arrivalRestart && latestSourceTimestamp > frame.sourceTimestamp &&
        latestSourceTimestamp - frame.sourceTimestamp >= config_.blurTimestampRestartThresholdMsec;
    if (arrivalRestart || timestampRestart) {
        latestSourceTimes_.remove(channelIndex);
        frameBuffer_->removeChannel(channelIndex);

        if (timestampRestart) {
            qWarning().noquote() << QStringLiteral("[MQTT BLUR] Channel %1 source timestamp restarted: %2 -> %3")
                                        .arg(channelIndex)
                                        .arg(latestSourceTimestamp)
                                        .arg(frame.sourceTimestamp);
        }
        latestSourceTimestamp = 0;
    }
    lastArrivalTimes_.insert(channelIndex, nowMsec);

    if (latestSourceTimestamp > 0 &&
        frame.sourceTimestamp < latestSourceTimestamp - config_.blurTimestampRestartThresholdMsec) {
        return;
    }

    latestSourceTimes_.insert(channelIndex, std::max(latestSourceTimestamp, frame.sourceTimestamp));
    if (frameBuffer_->submit(std::move(frame)) && !flushTimer_->isActive()) {
        flushTimer_->start();
    }
}

/**
 * @brief 같은 주기에 수신된 각 채널의 프레임을 timestamp 손실 없이 독립적으로 전달합니다.
 */
void BlurFrameDispatcher::flushPendingFrames() {
    if (!frameBuffer_) {
        return;
    }

    QVector<BlurFrameData> frames = frameBuffer_->takeLatestFrames();
    for (BlurFrameData& frame : frames) {
        emit frameReady(std::move(frame));
    }
    deliveredFrameCount_ += static_cast<quint64>(frames.size());

    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    if (lastStatisticsLogMsec_ == 0 || nowMsec - lastStatisticsLogMsec_ >= statisticsLogIntervalMsec) {
        const quint64 coalescedCount = frameBuffer_->takeCoalescedFrameCount();
        qDebug().noquote() << QStringLiteral("[MQTT BLUR DISPATCH] delivered=%1 coalesced=%2")
                                  .arg(deliveredFrameCount_)
                                  .arg(coalescedCount);
        lastStatisticsLogMsec_ = nowMsec;
        deliveredFrameCount_ = 0;
    }
}
