#include "network/realtime/BlurFrameDispatcher.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <utility>

#include "network/realtime/BlurFrameBuffer.h"

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
    clock_.start();
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
    reset();
}

/**
 * @brief 실행 상태는 유지하면서 대기 프레임과 채널별 timestamp 기준을 초기화합니다.
 */
void BlurFrameDispatcher::reset() {
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

    const qint64 nowMsec = qMax<qint64>(1, clock_.elapsed());
    const int channelIndex = frame.channelIndex;
    // lastArrivalTimes_는 "마지막 수신 시각"이 아니라 "마지막으로 승인한 프레임 시각"으로 사용합니다.
    // 오래된 프레임을 버릴 때 이 값을 갱신하면 stale 프레임이 계속 들어오는 동안 restart gap이
    // 영원히 성립하지 않아 채널이 복구 불가능한 상태에 빠질 수 있습니다.
    const qint64 lastAcceptedArrivalMsec = lastArrivalTimes_.value(channelIndex, 0);
    qint64 latestSourceTimestamp = latestSourceTimes_.value(channelIndex, 0);
    const bool acceptedFrameGapExpired =
        lastAcceptedArrivalMsec > 0 && nowMsec - lastAcceptedArrivalMsec >= config_.blurSourceRestartGapMsec;
    // 마지막 정상 프레임 이후 충분한 시간이 지났다면 timestamp 기준점을 버리고 현재 프레임부터 재동기화합니다.
    // 이 경로가 미래 timestamp 1개로 오염된 채널을 자동 복구합니다.
    if (acceptedFrameGapExpired) {
        latestSourceTimes_.remove(channelIndex);
        frameBuffer_->removeChannel(channelIndex);
        latestSourceTimestamp = 0;
    }

    // 기준 timestamp보다 지나치게 오래된 프레임은 버립니다.
    // 중요: reject된 프레임은 lastArrivalTimes_를 갱신하지 않습니다. 그래야 restart gap 이후 자동 복구됩니다.
    if (latestSourceTimestamp > 0 &&
        frame.sourceTimestamp < latestSourceTimestamp - config_.blurTimestampRestartThresholdMsec) {
        return;
    }

    // 승인된 프레임만 복구 감시 시각과 최신 timestamp를 전진시킵니다.
    lastArrivalTimes_.insert(channelIndex, nowMsec);
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

    const qint64 nowMsec = qMax<qint64>(1, clock_.elapsed());
    if (config_.logBlurDispatch &&
        (lastStatisticsLogMsec_ == 0 || nowMsec - lastStatisticsLogMsec_ >= statisticsLogIntervalMsec)) {
        const quint64 droppedCount = frameBuffer_->takeCoalescedFrameCount();
        qDebug().noquote() << QStringLiteral("[MQTT BLUR DISPATCH] delivered=%1 dropped=%2")
                                  .arg(deliveredFrameCount_)
                                  .arg(droppedCount);
        lastStatisticsLogMsec_ = nowMsec;
        deliveredFrameCount_ = 0;
    }
}
