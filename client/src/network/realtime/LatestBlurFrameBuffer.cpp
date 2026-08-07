#include "network/realtime/LatestBlurFrameBuffer.h"

#include <QMutexLocker>
#include <utility>

/**
 * @brief        채널별 블러 프레임을 bounded FIFO에 저장합니다.
 * @param frame  MQTT에서 검증된 블러 메타데이터 프레임
 * @return       소비 작업을 새로 예약해야 하면 true
 *
 * @details      클래스 이름은 기존 ABI/참조를 유지하지만 동작은 latest-only가 아닙니다.
 *               채널당 최대 32개를 보존하고 가득 차면 가장 오래된 프레임부터 버립니다.
 */
bool LatestBlurFrameBuffer::submit(BlurFrameData frame) {
    QMutexLocker locker(&mutex_);

    QQueue<BlurFrameData>& queue = pendingFrames_[frame.channelIndex];
    if (queue.size() >= maximumPendingFramesPerChannel) {
        queue.dequeue();
        ++coalescedFrameCount_;
    }
    queue.enqueue(std::move(frame));

    if (deliveryPending_) {
        return false;
    }

    deliveryPending_ = true;
    return true;
}

/**
 * @brief   저장된 모든 채널의 대기 프레임을 FIFO 순서로 꺼냅니다.
 * @return  채널별 도착 순서를 보존한 블러 프레임 목록
 */
QVector<BlurFrameData> LatestBlurFrameBuffer::takeLatestFrames() {
    QMutexLocker locker(&mutex_);

    qsizetype frameCount = 0;
    for (auto iterator = pendingFrames_.cbegin(); iterator != pendingFrames_.cend(); ++iterator) {
        frameCount += iterator.value().size();
    }

    QVector<BlurFrameData> frames;
    frames.reserve(frameCount);
    for (auto iterator = pendingFrames_.begin(); iterator != pendingFrames_.end(); ++iterator) {
        QQueue<BlurFrameData>& queue = iterator.value();
        while (!queue.isEmpty()) {
            frames.append(queue.dequeue());
        }
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
 * @brief   bounded queue가 가득 차 제거된 오래된 프레임 수를 읽고 카운터를 초기화합니다.
 * @return  직전 조회 이후 overflow로 제거된 프레임 수
 */
quint64 LatestBlurFrameBuffer::takeCoalescedFrameCount() {
    QMutexLocker locker(&mutex_);
    const quint64 count = coalescedFrameCount_;
    coalescedFrameCount_ = 0;
    return count;
}
