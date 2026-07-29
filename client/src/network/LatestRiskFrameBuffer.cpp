#include "network/LatestRiskFrameBuffer.h"

#include <QMutexLocker>
#include <utility>

/**
 * @brief        MQTT 작업 스레드에서 받은 최신 위험 프레임을 저장합니다.
 * @param frame  계약 검증과 timestamp 정렬을 통과한 위험 프레임
 * @return       GUI 전달 작업을 새로 예약해야 하면 true
 */
bool LatestRiskFrameBuffer::submit(RiskFrameData frame) {
    QMutexLocker locker(&mutex_);

    if (latestFrame_.has_value()) {
        ++coalescedFrameCount_;
    }
    latestFrame_ = std::move(frame);

    if (deliveryPending_) {
        return false;
    }

    deliveryPending_ = true;
    return true;
}

/**
 * @brief   가장 최근 위험 프레임을 꺼내고 다음 GUI 전달 예약을 허용합니다.
 * @return  대기 중인 최신 위험 프레임 또는 빈 값
 */
std::optional<RiskFrameData> LatestRiskFrameBuffer::takeLatestFrame() {
    QMutexLocker locker(&mutex_);

    std::optional<RiskFrameData> frame = std::move(latestFrame_);
    latestFrame_.reset();
    deliveryPending_ = false;
    return frame;
}

/** @brief GUI 전달 예약 실패 시 대기 상태를 초기화합니다. */
void LatestRiskFrameBuffer::cancelPendingDelivery() {
    QMutexLocker locker(&mutex_);
    latestFrame_.reset();
    deliveryPending_ = false;
}

/** @brief 대기 프레임과 병합 통계를 초기화합니다. */
void LatestRiskFrameBuffer::clear() {
    QMutexLocker locker(&mutex_);
    latestFrame_.reset();
    coalescedFrameCount_ = 0;
    deliveryPending_ = false;
}

/**
 * @brief   최신값으로 대체된 위험 프레임 수를 반환하고 통계를 초기화합니다.
 * @return  직전 조회 이후 병합된 위험 프레임 수
 */
quint64 LatestRiskFrameBuffer::takeCoalescedFrameCount() {
    QMutexLocker locker(&mutex_);
    const quint64 count = coalescedFrameCount_;
    coalescedFrameCount_ = 0;
    return count;
}
