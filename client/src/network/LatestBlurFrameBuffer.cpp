#include "network/LatestBlurFrameBuffer.h"

#include <QMutexLocker>
#include <utility>

/**
 * @brief        채널별 최신 블러 프레임을 저장합니다.
 * @param frame  MQTT에서 검증된 블러 메타데이터 프레임
 * @return       소비 작업을 새로 예약해야 하면 true
 */
bool LatestBlurFrameBuffer::submit(BlurFrameData frame) {
    QMutexLocker locker(&mutex_);

    if (latestFrames_.contains(frame.channelIndex)) {
        ++coalescedFrameCount_;
    }
    latestFrames_.insert(frame.channelIndex, std::move(frame));

    if (deliveryPending_) {
        return false;
    }

    deliveryPending_ = true;
    return true;
}

/**
 * @brief   저장된 채널별 최신 프레임을 꺼내고 다음 전달 예약을 허용합니다.
 * @return  채널당 최대 하나의 최신 블러 프레임 목록
 */
QVector<BlurFrameData> LatestBlurFrameBuffer::takeLatestFrames() {
    QMutexLocker locker(&mutex_);

    QVector<BlurFrameData> frames;
    frames.reserve(latestFrames_.size());
    for (auto iterator = latestFrames_.begin(); iterator != latestFrames_.end(); ++iterator) {
        frames.append(std::move(iterator.value()));
    }

    latestFrames_.clear();
    deliveryPending_ = false;
    return frames;
}

/**
 * @brief              지정한 채널의 대기 중인 블러 프레임을 제거합니다.
 * @param channelIndex 제거할 내부 채널 인덱스
 */
void LatestBlurFrameBuffer::removeChannel(int channelIndex) {
    QMutexLocker locker(&mutex_);
    latestFrames_.remove(channelIndex);
}

/**
 * @brief 예약에 실패한 전달 상태를 해제하여 다음 프레임이 다시 예약되게 합니다.
 */
void LatestBlurFrameBuffer::cancelPendingDelivery() {
    QMutexLocker locker(&mutex_);
    latestFrames_.clear();
    deliveryPending_ = false;
}

/**
 * @brief 모든 대기 프레임과 통계를 초기화합니다.
 */
void LatestBlurFrameBuffer::clear() {
    QMutexLocker locker(&mutex_);
    latestFrames_.clear();
    coalescedFrameCount_ = 0;
    deliveryPending_ = false;
}

/**
 * @brief   최신값으로 병합된 프레임 수를 읽고 카운터를 초기화합니다.
 * @return  직전 조회 이후 병합된 프레임 수
 */
quint64 LatestBlurFrameBuffer::takeCoalescedFrameCount() {
    QMutexLocker locker(&mutex_);
    const quint64 count = coalescedFrameCount_;
    coalescedFrameCount_ = 0;
    return count;
}
