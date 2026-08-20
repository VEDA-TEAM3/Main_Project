#include "network/realtime/LatestBlurFrameBuffer.h"

#include <QMutexLocker>
#include <utility>

/**
 * @brief        채널별 최신 블러 프레임만 저장합니다.
 * @param frame  MQTT에서 검증된 블러 메타데이터 프레임
 * @return       소비 작업을 새로 예약해야 하면 true
 *
 * @details      영상 queue가 버린 과거 프레임의 메타데이터를 UI와 receiver worker에
 *               뒤늦게 전달하지 않도록 아직 소비되지 않은 같은 채널 값은 교체합니다.
 */
bool LatestBlurFrameBuffer::submit(BlurFrameData frame) {
    QMutexLocker locker(&mutex_);

    const int channelIndex = frame.channelIndex;
    if (pendingFrames_.contains(channelIndex)) {
        ++coalescedFrameCount_;
    }
    pendingFrames_.insert(channelIndex, std::move(frame));

    if (deliveryPending_) {
        return false;
    }

    deliveryPending_ = true;
    return true;
}

/**
 * @brief   저장된 각 채널의 최신 프레임을 꺼냅니다.
 * @return  채널마다 최대 한 건인 블러 프레임 목록
 */
QVector<BlurFrameData> LatestBlurFrameBuffer::takeLatestFrames() {
    QMutexLocker locker(&mutex_);

    QVector<BlurFrameData> frames;
    frames.reserve(pendingFrames_.size());
    for (auto iterator = pendingFrames_.begin(); iterator != pendingFrames_.end(); ++iterator) {
        frames.append(std::move(iterator.value()));
    }

    pendingFrames_.clear();
    deliveryPending_ = false;
    return frames;
}

/**
 * @brief              지정한 채널의 대기 중인 블러 프레임을 제거합니다.
 * @param channelIndex 제거할 내부 채널 인덱스
 */
void LatestBlurFrameBuffer::removeChannel(int channelIndex) {
    QMutexLocker locker(&mutex_);
    pendingFrames_.remove(channelIndex);
}

/**
 * @brief 예약에 실패한 전달 상태만 해제합니다.
 *
 * @details 대기 프레임은 유지하여 다음 submit 시 다시 전달을 예약할 수 있게 합니다.
 */
void LatestBlurFrameBuffer::cancelPendingDelivery() {
    QMutexLocker locker(&mutex_);
    deliveryPending_ = false;
}

/**
 * @brief 모든 대기 프레임과 통계를 초기화합니다.
 */
void LatestBlurFrameBuffer::clear() {
    QMutexLocker locker(&mutex_);
    pendingFrames_.clear();
    coalescedFrameCount_ = 0;
    deliveryPending_ = false;
}

/**
 * @brief   최신값으로 교체된 오래된 프레임 수를 읽고 카운터를 초기화합니다.
 * @return  직전 조회 이후 교체된 프레임 수
 */
quint64 LatestBlurFrameBuffer::takeCoalescedFrameCount() {
    QMutexLocker locker(&mutex_);
    const quint64 count = coalescedFrameCount_;
    coalescedFrameCount_ = 0;
    return count;
}
