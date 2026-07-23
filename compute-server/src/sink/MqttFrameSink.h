#pragma once

/**
 * @file    MqttFrameSink.h
 * @brief   프레임 큐잉 + 백그라운드 발행을 담당하는 MQTT Sink 공통 기반
 *
 * @details
 * MqttBlurSink 와 MqttTopViewSink 가 공유하던 ~250줄(초기화/재시도/워커/드랍 집계/
 * 연결 콜백)이 이름만 다르고 동일했음 -> 연결 부분은 IMqttTransport 로, 큐잉 부분은
 * 이 템플릿으로 모음. 파생 클래스는 '이 프레임이 유효한가 / 어느 토픽으로 / 어떤 QoS로'
 * 만 정의하면 됨
 *
 * @note [ 스레드 모델 ]
 * - send()        : 파이프라인 스레드에서만 호출. 큐에 넣고 즉시 반환 (논블로킹, 예외 없음)
 * - workerLoop()  : Sink 마다 하나. 큐에서 꺼내 transport_->publish() 호출
 * - 연결 리스너   : mosquitto 네트워크 스레드. notify 만 함
 *
 * @note [ 왜 Sink 마다 큐/워커를 따로 두는가 ]
 * 커넥션은 공유하되 큐는 분리함. blur 발행이 밀린다고 risk(안전 크리티컬) 발행까지
 * 함께 지연되면 안 되기 때문
 *
 * @warning 큐가 가득 차면 drop-oldest. 실시간 좌표라 오래된 프레임보다 최신이 항상 유용함
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
     * @details 생성자에서 하지 않는 이유: 생성자 안에서 this 를 리스너로 넘기면
     *          파생 클래스가 아직 완성되지 않은 상태에서 콜백이 들어올 수 있음
     */
    void start() {
        listenerId_ = transport_->addConnectionListener([this](bool) {
            // queueMutex_ 를 한 번 잡았다 놓은 뒤 notify 하는 것이 핵심.
            // 그냥 notify 만 하면 "워커가 술어를 false 로 평가한 뒤 wait 에 진입하기 전"
            // 구간에 알림이 끼어들어 영영 깨어나지 못하는 lost wakeup 이 생김
            // (술어가 보는 isConnected() 는 queueMutex_ 밖의 atomic 이라 더더욱)
            { std::lock_guard<std::mutex> lock(queueMutex_); }
            queueChanged_.notify_all();
        });
        worker_ = std::thread(&MqttFrameSink::workerLoop, this);
    }

    void send(const T& frame) noexcept override {
        if (!prepare(frame, staging_)) {
            recordDrop("invalid frame");
            return;
        }

        if (!transport_->isReady()) {
            recordDrop("MQTT transport not ready");
            return;
        }

        try {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (stopping_) {
                    recordDrop("sink is stopping");
                    return;
                }
                if (queue_.size() >= maxQueueSize_) {
                    queue_.pop_front();
                    recordDrop("queue full; oldest frame removed");
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
     * @brief   입력 프레임을 검증하고 발행할 형태로 out 에 채움
     *
     * @param   in  파이프라인이 넘긴 원본 프레임
     * @param   out 큐에 넣을 프레임 (직전 호출에서 move 된 상태일 수 있으므로 전부 덮어쓸 것)
     * @return  발행 대상이면 true, 통째로 버릴 프레임이면 false
     *
     * @note    파이프라인 스레드에서만 호출됨
     */
    virtual bool prepare(const T& in, T& out) noexcept = 0;

    /// @brief 발행 성공 로그에 덧붙일 요약 (예: "objects=3")
    virtual std::string describe(const T& frame) const = 0;

    void recordDrop(const char* reason) noexcept {
        const std::uint64_t count = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            logError(iface_, "드랍 누적 " + std::to_string(count) +
                                 "건, 사유=" + std::string(reason != nullptr ? reason : "unknown"));
        }
    }

    /**
     * @brief   연결 리스너를 떼고, 워커를 멈추고, 큐를 비움 (멱등)
     *
     * @details
     * 파생 클래스의 소멸자에서 반드시 먼저 불러야 함
     * -- 워커가 publishFrame() 안에서 describe() 같은 가상 함수를 호출하므로,
     *    파생 멤버가 파괴된 뒤에도 워커가 돌면 순수 가상 호출/UAF 가 됨
     *
     * @note [ 리스너 해제가 먼저인 이유 ]
     * 리스너 람다는 this 를 캡처하는데 Sink 는 transport 보다 먼저 파괴됨
     * -- 떼지 않으면 이후 onDisconnect 가 죽은 Sink 를 호출함
     * -- queueMutex_ 를 잡기 '전에' 떼야 함: 리스너 콜백이 (transport 의 listenerMutex_ 를
     *    잡은 채) queueMutex_ 를 원하므로, 반대 순서로 잡으면 교착이 생김
     */
    void shutdown() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel))
            return;

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
        if (worker_.joinable())
            worker_.join();
    }

private:
    void workerLoop() noexcept {
        T frame;  // 루프 밖에 두어 벡터 capacity 를 재사용 (매 반복 재할당 방지)
        while (true) {
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueChanged_.wait(lock,
                                   [this] { return stopping_ || (transport_->isConnected() && !queue_.empty()); });

                if (stopping_)
                    return;

                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            publishFrame(frame);
        }
    }

    void publishFrame(const T& frame) noexcept {
        try {
            const std::string payload = veda::encode(frame);

            if (!transport_->publish(topic_, payload, qos_, false)) {
                recordDrop("transport publish failed");
                return;
            }

            const std::uint64_t count = publishedCount_.fetch_add(1, std::memory_order_relaxed) + 1;

            // 프레임마다 도는 정상 경로라 Debug -- describe() 문자열 조립까지 레벨로 걸러냄
            if (isLogEnabled(LogLevel::Debug)) {
                logDebug(iface_, "발행 성공 #" + std::to_string(count) + " topic=" + topic_ + " " + describe(frame) +
                                     " bytes=" + std::to_string(payload.size()));
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

    /// @brief prepare() 결과를 담는 staging 버퍼. send() 는 파이프라인 스레드 전용이라 락 불필요
    T staging_;

    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<T> queue_;
    std::thread worker_;
    bool stopping_ = false;  ///< queueMutex_ 로 보호

    /// @brief shutdown() 멱등 보장 (파생 소멸자 + 기반 소멸자에서 각각 호출됨)
    std::atomic_bool stopped_{false};

    IMqttTransport::ListenerId listenerId_ = IMqttTransport::kInvalidListener;

    std::atomic_uint64_t publishedCount_{0};
    std::atomic_uint64_t droppedCount_{0};
};
