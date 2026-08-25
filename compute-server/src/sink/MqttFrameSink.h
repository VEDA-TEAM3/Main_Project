#pragma once

/**
 * @file    MqttFrameSink.h
 * @brief   프레임 큐잉 + 백그라운드 발행을 담당하는 MQTT Sink 공통 기반
 *
 * @details
 * MqttBlurSink와 MqttTopViewSink가 공유하던 연결 부분은 IMqttTransport로, 큐잉 부분은
 * 이 템플릿으로 모음
 * 파생 클래스는 이 프레임이 유효한가 / 어느 토픽으로 / 어떤 QoS로만 정의하면 됨
 *
 * @note [ 스레드 모델 ]
 * - send()        : 파이프라인 스레드에서만 호출 (큐에 넣고 즉시 반환) (논블로킹, 예외 없음)
 * - workerLoop()  : Sink마다 하나 (큐에서 꺼내 transport_->publish() 호출)
 * - 연결 리스너   : mosquitto 네트워크 스레드 (notify 만 함)
 *
 * @note [ 왜 Sink마다 큐/워커를 따로 두는가 ]
 * 커넥션은 공유하되 큐는 분리함
 * Blur 발행이 밀린다고 Risk(안전 크리티컬) 발행까지 함께 지연되면 안 되기 때문
 *
 * @warning [ 최신 프레임 우선 — 두 지점에서 강제된다 ]
 * 실시간 좌표라 오래된 프레임보다 최신이 항상 유용하므로,
 *  1) send()       : 큐가 가득 차면 drop-oldest (메모리 상한)
 *  2) workerLoop() : 발행 직전 백로그를 합류시켜 가장 최신 한 장만 발행 (지연 상한)
 * 1)만 있으면 메모리는 잡히지만 지연은 잡히지 않는다. -- 큐 깊이가 maxQueueSize_에 고정된 채
 * FIFO로 빠지면서 소비자가 영구히 그만큼 과거를 보게 된다.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "Contract.h"
#include "Logger.h"
#include "interfaces/IMqttTransport.h"
#include "interfaces/ISink.h"

/**
 * @brief   MQTT 발행 Sink 공통 기반
 * @tparam  T 발행할 프레임 타입 (veda::TopViewFrame | veda::BlurFrame)
 */
template <typename T>
class MqttFrameSink : public ISink<T> {
public:
    static constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;

    /**
     * @param   transport     공유 MQTT 전송 계층
     * @param   topic         이 Sink 가 발행할 토픽 (채널이 프로세스당 고정이라 생성 시 1회만 계산)
     * @param   qos           발행 QoS
     * @param   maxQueueSize  큐 최대 길이 (최소 1로 clamp)
     * @param   iface         로그 태그
     */
    MqttFrameSink(std::shared_ptr<IMqttTransport> transport, std::string topic, int qos, std::size_t maxQueueSize,
                  const char* iface)
        : transport_(std::move(transport)),
          topic_(std::move(topic)),
          qos_(qos),
          maxQueueSize_(maxQueueSize < 1 ? 1 : maxQueueSize),
          iface_(iface) {}

    ~MqttFrameSink() override { shutdown(); }

    MqttFrameSink(const MqttFrameSink&) = delete;
    MqttFrameSink& operator=(const MqttFrameSink&) = delete;

    /**
     * @brief   워커 스레드를 띄우고 전송 계층의 연결 이벤트를 구독
     * @details 생성자에서 하지 않는 이유: 생성자 안에서 this를 리스너로 넘기면
     *          파생 클래스가 아직 완성되지 않은 상태에서 콜백이 들어올 수 있음
     */
    void start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        if (stopped_.load(std::memory_order_acquire)) {
            started_.store(false, std::memory_order_release);
            return;
        }

        try {
            listenerId_ = transport_->addConnectionListener([this](bool) {
                // queueMutex_를 한 번 잡았다 놓은 뒤 notify 하는 것이 핵심
                // 그냥 notify만 하면 워커가 술어를 false로 평가한 뒤 wait 에 진입하기 전
                // 구간에 알림이 끼어들어 영영 깨어나지 못하는 lost wakeup이 생김
                // (술어가 보는 isConnected()는 queueMutex_ 밖의 atomic이라 더더욱)
                { std::lock_guard<std::mutex> lock(queueMutex_); }
                queueChanged_.notify_all();
            });
            worker_ = std::thread(&MqttFrameSink::workerLoop, this);
        } catch (...) {
            if (listenerId_ != IMqttTransport::kInvalidListener) {
                transport_->removeConnectionListener(listenerId_);
            }

            listenerId_ = IMqttTransport::kInvalidListener;
            started_.store(false, std::memory_order_release);
            throw;
        }
    }

    void send(const T& frame) noexcept override {
        try {
            if (!prepare(frame, staging_)) {
                recordDrop("invalid frame");
                return;
            }

            if (!transport_->isReady()) {
                recordDrop("MQTT transport not ready");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (stopping_) {
                    recordDrop("sink is stopping");
                    return;
                }
                if (queue_.size() >= maxQueueSize_) {
                    queue_.pop_front();

                    // 큐가 가득 찬 이유를 구분해서 남긴다.
                    //   - 미연결  : 브로커 주소/포트/TLS/도달성을 확인해야 함 (코드 문제 아님)
                    //   - 백로그  : 발행이 유입 속도를 못 따라감 → 큐 길이/QoS/대역폭 검토
                    //
                    // isConnected()를 queueMutex_안에서 부르는 것은 안전하다.
                    // IMqttTransport 계약이 이 함수를 락 프리로 못박고 있기 때문
                    recordDrop(transport_->isConnected() ? "queue full; publish backlog (broker/network slow)"
                                                         : "queue full; broker NOT connected (worker parked)");
                }
                queue_.push_back(std::move(staging_));
            }
            queueChanged_.notify_one();
        } catch (const std::exception& error) {
            recordDrop(error.what());
        } catch (...) {
            recordDrop("unknown enqueue exception");
        }
    }

    std::uint64_t publishedCount() const noexcept { return publishedCount_.load(std::memory_order_relaxed); }
    std::uint64_t droppedCount() const noexcept { return droppedCount_.load(std::memory_order_relaxed); }

protected:
    /**
     * @brief   입력 프레임을 검증하고 발행할 형태로 out에 채움
     *
     * @param   in  파이프라인이 넘긴 원본 프레임
     * @param   out 큐에 넣을 프레임 (직전 호출에서 move된 상태이므로 전부 덮어쓸 것)
     * @return  발행 대상이면 true, 통째로 버릴 프레임이면 false
     *
     * @note    파이프라인 스레드에서만 호출됨
     *
     * @warning [ out의 capacity는 재사용되지 않는다 — 프레임당 힙 할당 1회가 발생한다 ]
     * send()가 staging_를 큐로 move하므로(queue_.push_back(std::move(staging_))), 다음 호출에서
     * out은 언제나 capacity 0인 상태로 들어온다. 즉 여기서 assign/reserve를 어떻게 쓰든
     * 벡터 버퍼는 매번 새로 할당된다. (실측 약 1.1회/프레임)
     *
     * 버퍼는 파이프라인 → 큐 → 워커 → 소멸의 단방향으로만 흐른다. RtspOnvifSourceV2::next()가
     * std::swap으로 빈 버퍼를 링 슬롯에 돌려주는 것과 달리, 여기에는 되돌리는 경로가 없다.
     *
     * 이는 의도된 트레이드오프다. -- 버퍼를 순환시키려면 워커가 발행을 마친 프레임을 send()쪽으로
     * 돌려주는 락으로 보호되는 핸드오프가 하나 더 필요해진다. 발행 빈도가 5fps × 2 Sink라
     * 절약되는 것은 초당 10여 회의 할당뿐인데, 그 대가로 이 계층에서 가장 중요한 자산인
     * 동시성 구조의 단순함을 잃는다. 작은 할당 비용을 내고 동시성을 단순하게 유지하는 쪽을 택했다.
     *
     * @note 반대로 publishFrame()의 payloadBuf_는 워커 밖으로 소유권이 나가지 않으므로
     *       clear()의 capacity 유지가 실제로 성립한다.
     */
    virtual bool prepare(const T& in, T& out) = 0;

    /// @brief 발행 성공 로그에 덧붙일 요약
    virtual std::string describe(const T& frame) const = 0;

    void recordDrop(const char* reason) noexcept {
        const std::uint64_t count = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            try {
                logError(iface_, "드랍 누적 " + std::to_string(count) +
                                     "건, 사유=" + std::string(reason != nullptr ? reason : "unknown"));
            } catch (...) {
            }
        }
    }

    /**
     * @brief   연결 리스너를 떼고, 워커를 멈추고, 큐를 비움 (멱등)
     *
     * @details
     * 파생 클래스의 소멸자에서 반드시 먼저 불러야 함
     * -- 워커가 publishFrame()안에서 describe()같은 가상 함수를 호출하므로,
     *    파생 멤버가 파괴된 뒤에도 워커가 돌면 순수 가상 호출/UAF가 됨
     *
     * @note [ 리스너 해제가 먼저인 이유 ]
     * 리스너 람다는 this를 캡처하는데 Sink는 transport보다 먼저 파괴됨
     * -- 떼지 않으면 이후 onDisconnect가 죽은 Sink를 호출함
     * -- queueMutex_를 잡기 전에 떼야 함: 리스너 콜백이 (transport의 listenerMutex_를
     *    잡은 채) queueMutex_를 원하므로, 반대 순서로 잡으면 교착이 생김
     */
    void shutdown() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        transport_->removeConnectionListener(listenerId_);
        listenerId_ = IMqttTransport::kInvalidListener;

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = true;
            while (!queue_.empty()) {
                queue_.pop_front();
                recordDrop("shutdown");
            }
        }
        queueChanged_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void workerLoop() noexcept {
        // 루프 밖에 두는 것은 객체 재구축을 아끼기 위함이지 capacity 재사용이 아니다.
        // frame = std::move(queue_.front())은 frame의 기존 버퍼를 해제하고 큐 원소의 버퍼를
        // 넘겨받으므로, 반복마다 이전 capacity는 버려진다.
        T frame;
        while (true) {
            std::size_t superseded = 0;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueChanged_.wait(lock,
                                   [this] { return stopping_ || (transport_->isConnected() && !queue_.empty()); });

                if (stopping_) {
                    return;
                }

                // [지연 합류] 백로그가 있으면 가장 최신 프레임만 발행하고 나머지는 버린다.
                //
                // TopViewFrame/BlurFrame 은 델타가 아니라 그 시각의 전체 상태 스냅샷이다.
                // 프레임 N+1은 프레임 N을 완전히 대체하므로, 밀린 프레임을 FIFO로 순서대로
                // 내보내면 정보는 하나도 더 주지 못하면서 (백로그 깊이 x 프레임 간격) 만큼
                // 지연만 그대로 쌓인다.
                //
                // 특히 drop-oldest 와 만나면 지연이 고정된다: 큐가 한 번 포화되면 send()가
                // 앞에서 하나 버리고 뒤에 하나 넣으므로 깊이가 maxQueueSize_에 고정되고,
                // FIFO로 빼는 한 소비자는 영영 maxQueueSize_ 프레임만큼 과거를 본다.
                // (5fps + 큐 8 => 1.6초 고정 지연)
                // 생산 속도가 소비 속도 이상인 동안 이 지연은 저절로 회복되지 않는다.
                //
                // Blur 경로에서는 이게 지연을 넘어 정확성 문제다.
                // -- 1.6초 지난 블러 박스는 현재 영상의 엉뚱한 곳을 가리므로 얼굴이 그대로 노출된다.
                //
                // 버리는 개수는 FIFO와 같다. (생산-소비 속도 차이가 결정) 어느 프레임을
                // 버리느냐만 달라지며, 항상 최신을 남긴다.
                superseded = queue_.size() - 1;
                if (superseded > 0) {
                    frame = std::move(queue_.back());
                    queue_.clear();
                } else {
                    frame = std::move(queue_.front());
                    queue_.pop_front();
                }
            }

            // 락 밖에서 계상 (recordDrop은 atomic + rate-limit이라 락이 필요 없음)
            // superseded는 maxQueueSize_로 유계이며, 정상 운영에서는 0이다.
            for (std::size_t i = 0; i < superseded; ++i) {
                recordDrop("superseded by newer frame (latency coalescing)");
            }

            publishFrame(frame);
        }
    }

    void publishFrame(const T& frame) noexcept {
        try {
            // Zero-DOM 직렬화: nlohmann DOM 트리를 프레임마다 새로 만들지 않고 재사용 버퍼에 직접 append한다.
            // payloadBuf_는 clear()로 capacity를 유지하므로 warmup 이후 힙 할당이 없음
            veda::encodeInto(frame, payloadBuf_);
            if (payloadBuf_.size() > kMaxPayloadBytes) {
                recordDrop("serialized payload too large");
                return;
            }

            if (!transport_->publish(topic_, payloadBuf_, qos_, false)) {
                recordDrop("transport publish failed");
                return;
            }

            const std::uint64_t count = publishedCount_.fetch_add(1, std::memory_order_relaxed) + 1;

            // 프레임마다 도는 정상 경로라 Debug
            if (isLogEnabled(LogLevel::Debug)) {
                logDebug(iface_, "발행 성공 #" + std::to_string(count) + " topic=" + topic_ + " " + describe(frame) +
                                     " bytes=" + std::to_string(payloadBuf_.size()));
            }
        } catch (const std::exception& error) {
            recordDrop(error.what());
        } catch (...) {
            recordDrop("unknown publish exception");
        }
    }

    std::shared_ptr<IMqttTransport> transport_;
    std::string topic_;  ///< 채널이 고정이라 생성 시 1회 계산 (발행마다 문자열을 다시 만들지 않음)
    int qos_;
    std::size_t maxQueueSize_;
    const char* iface_;

    /// @brief prepare() 결과를 담는 staging 버퍼 (send()는 파이프라인 스레드 전용이라 락 불필요)
    T staging_;

    /// @brief publishFrame 전용 재사용 직렬화 버퍼 (worker 스레드에서만 접근)
    ///        clear()로 capacity를 유지 → 프레임마다의 DOM/문자열 힙 할당 제거 (zero-DOM 직렬화)
    std::string payloadBuf_;

    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<T> queue_;
    std::thread worker_;
    bool stopping_ = false;  ///< queueMutex_로 보호

    /// @brief shutdown() 멱등 보장 (파생 소멸자 + 기반 소멸자에서 각각 호출됨)
    std::atomic_bool stopped_{false};
    std::atomic_bool started_{false};

    IMqttTransport::ListenerId listenerId_ = IMqttTransport::kInvalidListener;

    std::atomic_uint64_t publishedCount_{0};
    std::atomic_uint64_t droppedCount_{0};
};