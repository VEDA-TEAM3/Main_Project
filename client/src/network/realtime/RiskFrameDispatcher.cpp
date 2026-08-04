#include "network/realtime/RiskFrameDispatcher.h"

#include <QDebug>
#include <QTimer>
#include <utility>

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
    latestSourceTimestamp_ = 0;
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

    // control-server가 싣는 RiskFrame.ts는 윈도우에 모인 채널 관측 중 가장 오래된 값이라
    // 채널 구성이 바뀌면 뒤로 갈 수 있다. ts로 순서를 매기면 그 구간의 프레임이 통째로 버려져
    // 화면이 멈췄다가 튄다. 순서는 도착 순(QoS 1)으로 두고 ts는 재전송 중복 제거에만 쓴다
    if (frame.sourceTimestamp == latestSourceTimestamp_) {
        return;
    }

    latestSourceTimestamp_ = frame.sourceTimestamp;
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
