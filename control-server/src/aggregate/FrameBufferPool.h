#pragma once

/**
 * @file    FrameBufferPool.h
 * @brief   윈도우 마감용 프레임 버퍼 메모리 풀 (핫패스 무할당)
 *
 * @details
 * 집계기는 윈도우가 닫힐 때마다 '채널당 최신 프레임 묶음'을 콜백에 넘긴다. 이때 매번
 * std::vector<TopViewFrame>를 새로 만들면 (1) 외곽 벡터 1회 + (2) 프레임마다 objects
 * 벡터 1회씩 힙 할당이 발생한다. 12채널·10Hz 기준 초당 약 130회다.
 *
 * 이 풀은 그 버퍼를 '해제하지 않고 돌려쓴다'. acquire()/release() 사이에서 버퍼의
 * capacity 는 그대로 유지되므로, warmup 이후에는 할당이 0이 된다.
 *
 * @note [ 왜 lock-free 가 아닌가 ]
 * acquire/release 는 윈도우 마감마다(기본 10Hz) 한 번씩만 일어난다. 경합이 사실상 없어
 * lock-free 자료구조의 복잡도를 치를 이유가 없다 -- 라즈베리파이에서는 단순한 mutex 가 낫다.
 *
 * @warning 풀이 비면 '실패' 대신 새 버퍼를 만들어 반환한다(fail-open). 집계는 실시간
 *          경로이므로 버퍼가 없다고 프레임을 버리는 것이 더 나쁘다. 대신 그 사실을
 *          카운트해 두어 풀 크기가 부족한지 진단할 수 있게 한다.
 */

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "Contract.h"

class FrameBufferPool {
public:
    using Buffer = std::vector<veda::TopViewFrame>;

    /**
     * @param poolSize        미리 확보해 둘 버퍼 개수
     * @param framesPerBuffer 버퍼 1개가 담을 최대 프레임 수 (= channelCount)
     */
    FrameBufferPool(std::size_t poolSize, std::size_t framesPerBuffer) : framesPerBuffer_(framesPerBuffer) {
        free_.reserve(poolSize);
        for (std::size_t i = 0; i < poolSize; ++i) {
            Buffer buf;
            // 외곽 벡터를 미리 키워 둔다 -> 이후 resize(n<=framesPerBuffer) 는 재할당이 없다
            buf.reserve(framesPerBuffer);
            free_.push_back(std::move(buf));
        }
    }

    /**
     * @brief   버퍼 1개를 빌린다. 반드시 release() 로 돌려줄 것
     * @details 풀이 비었으면 새로 만들어 반환한다(fail-open). exhaustedCount() 로 감지 가능
     */
    Buffer acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty()) {
            ++exhaustedCount_;
            Buffer buf;
            buf.reserve(framesPerBuffer_);
            return buf;
        }
        Buffer buf = std::move(free_.back());
        free_.pop_back();
        return buf;
    }

    /**
     * @brief   버퍼를 반납한다
     *
     * @warning clear() 를 하지 않는다. clear() 는 TopViewFrame 원소를 파괴하면서 그 안의
     *          objects 벡터 버퍼까지 해제해 버리므로, 다음 윈도우에서 다시 할당하게 된다.
     *          원소를 살려 둔 채 돌려주는 것이 이 풀의 핵심이다 -- 호출자가 resize() 로
     *          길이만 조절해 재사용한다.
     */
    void release(Buffer&& buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(std::move(buf));
    }

    /// @brief 풀이 비어 새로 할당한 횟수 (0 이 아니면 poolSize 를 늘릴 것)
    std::size_t exhaustedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return exhaustedCount_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Buffer> free_;
    std::size_t framesPerBuffer_;
    std::size_t exhaustedCount_ = 0;
};
