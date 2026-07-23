/**
 * @file    sink_lifecycle.cpp
 * @brief   MqttFrameSink / IMqttTransport 수명·동시성 회귀 테스트
 *
 * @details
 * FakeTransport 를 주입해 연결/끊김을 인위적으로 만들고 다음을 확인한다.
 *  1) 연결 전 큐잉 -> 연결 시 워커가 깨어나 발행 (lost wakeup 회귀)
 *  2) 큐 포화 시 drop-oldest, 토픽 캐싱
 *  3) Sink 파괴 후 transport 가 이벤트를 쏴도 죽은 객체를 부르지 않음 (use-after-free 회귀)
 *  4) blur 대상 중 잘못된 것만 걸러내고 프레임은 살림
 *
 * @par [ 빌드 & 실행 ]
 * @code
 * cmake -B build && cmake --build build --target sink-lifecycle
 * ./build/sink-lifecycle
 * @endcode
 *
 * @note [ 새니타이저와 함께 돌리기 ]
 * 이 테스트가 잡는 결함은 대부분 경합이라 새니타이저를 같이 쓰는 게 좋다.
 * @code
 * cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1"
 * cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
 * @endcode
 * 다만 3)의 use-after-free 는 ASAN 이 못 잡는다 -- 죽은 객체를 건드리는 지점이
 * 계측되지 않는 pthread_mutex_lock 내부라서. 리스너 해제 여부를 직접 단언하는
 * listenerCount() 검사가 실질적인 안전망이다.
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/AppConfig.h"
#include "sink/MqttBlurSink.h"
#include "sink/MqttTopViewSink.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

void section(const std::string& title) { std::printf("\n=== %s ===\n", title.c_str()); }

/**
 * @brief   브로커 없이 Sink 를 돌리기 위한 가짜 전송 계층
 * @details IMqttTransport 로 추상화해둔 덕에 libmosquitto 없이도 Sink 로직을 검증할 수 있음
 */
class FakeTransport final : public IMqttTransport {
public:
    ListenerId addConnectionListener(std::function<void(bool)> listener) override {
        std::lock_guard<std::mutex> lock(listenerMutex_);
        const ListenerId id = nextId_++;
        listeners_.emplace_back(id, std::move(listener));
        return id;
    }

    void removeConnectionListener(ListenerId id) noexcept override {
        std::lock_guard<std::mutex> lock(listenerMutex_);
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (it->first == id) {
                listeners_.erase(it);
                return;
            }
        }
    }

    bool start() noexcept override {
        ready_.store(true);
        return true;
    }

    void stop() noexcept override { ready_.store(false); }

    bool publish(std::string_view topic, std::string_view, int, bool) noexcept override {
        std::lock_guard<std::mutex> lock(publishMutex_);
        publishedTopics_.emplace_back(topic);
        return true;
    }

    bool isReady() const noexcept override { return ready_.load(); }
    bool isConnected() const noexcept override { return connected_.load(); }

    /// @brief 브로커 연결/끊김을 흉내내어 리스너를 호출
    void simulateConnection(bool connected) {
        connected_.store(connected);
        std::lock_guard<std::mutex> lock(listenerMutex_);
        for (const auto& entry : listeners_)
            entry.second(connected);
    }

    std::size_t publishedCount() const {
        std::lock_guard<std::mutex> lock(publishMutex_);
        return publishedTopics_.size();
    }

    std::size_t listenerCount() const {
        std::lock_guard<std::mutex> lock(listenerMutex_);
        return listeners_.size();
    }

    std::string lastTopic() const {
        std::lock_guard<std::mutex> lock(publishMutex_);
        return publishedTopics_.empty() ? std::string{} : publishedTopics_.back();
    }

private:
    mutable std::mutex listenerMutex_;
    mutable std::mutex publishMutex_;
    std::vector<std::pair<ListenerId, std::function<void(bool)>>> listeners_;
    std::vector<std::string> publishedTopics_;
    ListenerId nextId_ = 1;
    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
};

AppConfig makeConfig() {
    AppConfig config;
    config.channelId = 2;
    config.channelCount = 4;
    config.mqttTopViewMaxQueueSize = 4;
    config.mqttBlurMaxQueueSize = 4;
    return config;
}

veda::TopViewFrame makeTopViewFrame(veda::TimestampMs ts) {
    veda::TopViewFrame frame;
    frame.ts = ts;
    frame.ch = 2;
    frame.objects.push_back(veda::TopViewObject{1, veda::ObjectClass::Human, veda::LocalPoint{1.0, 2.0}, false});
    return frame;
}

/// @brief 조건이 참이 될 때까지 최대 timeout 만큼 폴링하며 기다림
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// @brief 테스트 대상이 남기는 로그를 삼켜서 결과 출력이 묻히지 않도록 함
class LogSilencer {
public:
    LogSilencer() : out_(std::cout.rdbuf(nullptr)), err_(std::cerr.rdbuf(nullptr)) {}
    ~LogSilencer() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    LogSilencer(const LogSilencer&) = delete;
    LogSilencer& operator=(const LogSilencer&) = delete;

private:
    std::streambuf* out_;
    std::streambuf* err_;
};

constexpr int kWakeupAttempts = 50;

}  // namespace

int main() {
    const AppConfig config = makeConfig();
    LogSilencer silencer;

    // -----------------------------------------------------------------------
    section("1) 연결 전 큐잉 -> 연결 시 깨어나 발행 (lost wakeup 회귀)");
    // -----------------------------------------------------------------------
    // 워커가 '술어를 false 로 평가한 뒤 wait 진입하기 전' 구간에 연결 알림이 끼어들면
    // 예전 구현은 알림을 놓치고 영영 깨어나지 못했음. 경합 구간이 좁아서 반복 시도로 노림
    {
        bool allWokeUp = true;
        for (int attempt = 1; attempt <= kWakeupAttempts && allWokeUp; ++attempt) {
            auto transport = std::make_shared<FakeTransport>();
            transport->start();
            auto sink = std::make_shared<MqttTopViewSink>(transport, config);
            sink->start();

            std::thread connector([&transport] { transport->simulateConnection(true); });
            sink->send(makeTopViewFrame(1000 + attempt));
            connector.join();

            if (!waitFor([&transport] { return transport->publishedCount() >= 1; })) {
                check(false, "시도 " + std::to_string(attempt) + "회차에서 워커가 깨어나지 못함 (lost wakeup)");
                allWokeUp = false;
            }
        }
        if (allWokeUp)
            check(true, std::to_string(kWakeupAttempts) + "회 반복 모두 연결 직후 발행됨 (lost wakeup 없음)");
    }

    // -----------------------------------------------------------------------
    section("2) drop-oldest / 토픽 캐싱");
    // -----------------------------------------------------------------------
    {
        auto transport = std::make_shared<FakeTransport>();
        transport->start();
        auto sink = std::make_shared<MqttTopViewSink>(transport, config);
        sink->start();

        // 아직 연결 전이라 워커는 대기 -> 큐만 쌓임 (maxQueueSize=4)
        for (int i = 0; i < 10; ++i)
            sink->send(makeTopViewFrame(2000 + i));

        check(sink->droppedCount() >= 6,
              "큐 초과분이 drop-oldest 로 버려짐 (dropped=" + std::to_string(sink->droppedCount()) + ")");

        transport->simulateConnection(true);
        check(waitFor([&transport] { return transport->publishedCount() >= 4; }), "연결 후 큐에 남은 4개가 발행됨");
        check(transport->lastTopic() == "veda/ch/2/topview",
              "토픽이 channelId 로 1회 계산되어 캐싱됨 (" + transport->lastTopic() + ")");
    }

    // -----------------------------------------------------------------------
    section("3) Sink 파괴 후 연결 이벤트 (use-after-free 회귀)");
    // -----------------------------------------------------------------------
    // 리스너 람다는 Sink 의 this 를 캡처하는데 Sink 가 transport 보다 먼저 죽음
    // -> 파괴 시 리스너를 떼지 않으면 이후 이벤트가 죽은 객체를 호출함
    {
        auto transport = std::make_shared<FakeTransport>();
        transport->start();

        {
            auto sink = std::make_shared<MqttTopViewSink>(transport, config);
            sink->start();
            check(transport->listenerCount() == 1, "start() 가 연결 리스너를 등록함");

            transport->simulateConnection(true);
            sink->send(makeTopViewFrame(3000));
            waitFor([&transport] { return transport->publishedCount() >= 1; });
        }  // sink 파괴

        check(transport->listenerCount() == 0, "Sink 파괴 시 연결 리스너가 해제됨");

        transport->simulateConnection(false);
        transport->simulateConnection(true);
        check(true, "Sink 파괴 후 연결/끊김 이벤트가 와도 크래시 없음");
    }

    // -----------------------------------------------------------------------
    section("4) blur 대상 부분 필터링");
    // -----------------------------------------------------------------------
    // 얼굴 하나가 이상하다고 같은 프레임의 다른 blur 까지 버리면 안 됨
    {
        auto transport = std::make_shared<FakeTransport>();
        transport->start();
        auto sink = std::make_shared<MqttBlurSink>(transport, config);
        sink->start();
        transport->simulateConnection(true);

        veda::BlurFrame frame;
        frame.ts = 4000;
        frame.ch = 2;
        frame.blurs.push_back(veda::BlurTarget{1, veda::ObjectClass::Head, veda::NormRect{0.1, 0.1, 0.2, 0.2}});
        frame.blurs.push_back(veda::BlurTarget{2, veda::ObjectClass::Human, veda::NormRect{0.3, 0.3, 0.4, 0.4}});
        frame.blurs.push_back(veda::BlurTarget{3, veda::ObjectClass::LicensePlate, veda::NormRect{0.5, 0.5, 0.6, 0.6}});
        sink->send(frame);

        check(waitFor([&transport] { return transport->publishedCount() >= 1; }),
              "잘못된 blur 대상이 섞여 있어도 프레임 자체는 발행됨");
        check(transport->lastTopic() == "veda/ch/2/blur", "blur 토픽이 맞음 (" + transport->lastTopic() + ")");
        check(sink->droppedCount() == 1,
              "Human 클래스 대상 1개만 걸러짐 (dropped=" + std::to_string(sink->droppedCount()) + ")");

        // 채널 범위를 벗어난 프레임은 통째로 폐기
        veda::BlurFrame badChannel = frame;
        badChannel.ch = 99;
        const std::size_t before = transport->publishedCount();
        sink->send(badChannel);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(transport->publishedCount() == before, "channelCount 밖 채널의 프레임은 발행되지 않음");
    }

    section("결과");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
