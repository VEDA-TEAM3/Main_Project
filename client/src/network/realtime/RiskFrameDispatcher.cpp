#include "network/realtime/RiskFrameDispatcher.h"

#include <QDebug>
#include <QTimer>
#include <utility>

namespace {
bool sameRiskObject(const RiskObjectData& first, const RiskObjectData& second) {
    return first.globalId == second.globalId && first.objectClass == second.objectClass &&
           first.worldPosition == second.worldPosition && first.riskLevel == second.riskLevel &&
           first.nearestId == second.nearestId && first.distance == second.distance && first.zoneId == second.zoneId;
}

bool sameRiskFrame(const RiskFrameData& first, const RiskFrameData& second) {
    if (first.sourceTimestamp != second.sourceTimestamp || first.riskLevel != second.riskLevel ||
        first.objects.size() != second.objects.size()) {
        return false;
    }

    for (qsizetype index = 0; index < first.objects.size(); ++index) {
        if (!sameRiskObject(first.objects.at(index), second.objects.at(index))) {
            return false;
        }
    }
    return true;
}
}  // namespace

/**
 * @brief         통합 위험 프레임의 최신값 병합 디스패처를 생성합니다.
 * @param config  JSON 검증을 통과한 MQTT dispatcher 설정
 * @param parent  Qt 객체 소유권을 연결할 부모 객체
 */
RiskFrameDispatcher::RiskFrameDispatcher(MqttDispatcherConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    clock_.start();
    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(config_.riskFlushIntervalMsec);
    flushTimer_->setSingleShot(true);
    flushTimer_->setTimerType(Qt::PreciseTimer);
    connect(flushTimer_, &QTimer::timeout, this, &RiskFrameDispatcher::flushPendingFrame);
}

/** @brief 위험 프레임 전달을 시작합니다. */
void RiskFrameDispatcher::start() {
    reset();
    running_ = true;
}

/** @brief 예약된 위험 프레임과 시간 상태를 정리합니다. */
void RiskFrameDispatcher::stop() {
    running_ = false;
    reset();
}

/** @brief MQTT 재연결 또는 송신기 재시작 전에 남은 프레임 순서 상태를 초기화합니다. */
void RiskFrameDispatcher::reset() {
    flushTimer_->stop();
    pendingFrame_ = {};
    lastAcceptedFrame_.reset();
    lastArrivalMsec_ = 0;
    debugWindowStartMsec_ = 0;
    debugReceivedCount_ = 0;
    debugCoalescedCount_ = 0;
    debugDeliveredCount_ = 0;
    hasPendingFrame_ = false;
}

/**
 * @brief        가장 최신인 통합 위험 프레임만 전달 대기열에 보관합니다.
 * @param frame  계약 검증을 통과한 위험 프레임
 */
void RiskFrameDispatcher::submitFrame(RiskFrameData frame) {
    if (!running_ || frame.sourceTimestamp <= 0) {
        return;
    }

    const qint64 nowMsec = qMax<qint64>(1, clock_.elapsed());
    if (lastArrivalMsec_ > 0 && nowMsec - lastArrivalMsec_ > config_.riskSourceRestartGapMsec) {
        reset();
    }
    lastArrivalMsec_ = nowMsec;

    // RiskFrame.ts는 frame sequence가 아니므로 timestamp만 같다는 이유로 버리지 않습니다.
    // QoS 0을 유지하되, 직전 승인 프레임과 timestamp/상태/객체 내용이 모두 같은 실제 재전송만 제거합니다.
    // 같은 ts에 객체/위험 상태가 갱신된 프레임은 정상적으로 통과합니다.
    if (lastAcceptedFrame_.has_value() && sameRiskFrame(frame, *lastAcceptedFrame_)) {
        return;
    }

    lastAcceptedFrame_ = frame;
    if (config_.logRiskDispatch) {
        ++debugReceivedCount_;
        if (hasPendingFrame_) {
            // 아직 전달되지 않은 프레임을 덮어쓴다 = 지도가 이 프레임을 영영 못 본다
            ++debugCoalescedCount_;
        }
        if (debugWindowStartMsec_ <= 0) {
            debugWindowStartMsec_ = nowMsec;
        } else if (nowMsec - debugWindowStartMsec_ >= 1000) {
            qInfo().noquote() << QStringLiteral("[TV] disp rx=%1 tx=%2 coal=%3")
                                     .arg(debugReceivedCount_)
                                     .arg(debugDeliveredCount_)
                                     .arg(debugCoalescedCount_);
            debugWindowStartMsec_ = nowMsec;
            debugReceivedCount_ = 0;
            debugDeliveredCount_ = 0;
            debugCoalescedCount_ = 0;
        }
    }

    pendingFrame_ = std::move(frame);
    hasPendingFrame_ = true;
    if (!flushTimer_->isActive()) {
        flushTimer_->start();
    }
}

/** @brief 현재 전달 구간에서 가장 최신인 위험 프레임 하나를 방출합니다. */
void RiskFrameDispatcher::flushPendingFrame() {
    if (!hasPendingFrame_) {
        return;
    }

    hasPendingFrame_ = false;
    ++debugDeliveredCount_;
    emit frameReady(std::exchange(pendingFrame_, {}));
}
