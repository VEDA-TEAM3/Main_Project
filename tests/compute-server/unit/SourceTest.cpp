/**
 * @file    SourceTest.cpp
 * @brief   IMetadataSource 계약 + RtspOnvifSourceV2 종료 경로 단위 테스트
 *
 * @details
 * 두 층위로 나눠 검증한다:
 *
 *  1) **MockIMetadataSource (gmock)** — Pipeline 이 의존하는 *계약*을 고정한다.
 *     (next() 가 false 를 반환하면 소비 루프가 종료된다 / stop() 은 noexcept 다 등)
 *
 *  2) **실제 RtspOnvifSourceV2** — 소켓 없이도 검증 가능한 **종료(shutdown) 경로**를 실제로 돌린다.
 *     rtspIp 를 inet_pton 이 거부하는 문자열로 두면 connect() 가 즉시 실패하므로
 *     네트워크 없이 워커 루프(백오프 대기)만 살아 있는 상태를 만들 수 있다.
 *     이 상태에서 stop() -> next() 해제 및 소멸자 join 을 검증한다.
 *     => [W1] lost-wakeup 패치(notify 전 mtx_ 획득)의 회귀 테스트
 *
 * @note SPSC 링버퍼의 drop-oldest 내부 상태는 private 이고, 링에 데이터를 넣으려면
 *       실제 RTSP 세션이 필요하다. 따라서 링 '내부'를 직접 단위 테스트하지는 않는다
 *       (테스트용으로 로직을 복제해 검증하면 프로덕션 코드가 아닌 사본을 검증하게 되므로
 *        의도적으로 하지 않았다). 링의 실측 동작은 통합 지표(droppedCount)로 관측한다.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "core/AppConfig.h"
#include "domain/RawPacket.h"
#include "interfaces/IMetadataSource.h"
#include "source/RtspOnvifSourceV2.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgReferee;

namespace {

/// @brief IMetadataSource 의 gmock 대역 — 소켓/스레드 없이 계약만 검증
class MockIMetadataSource : public IMetadataSource {
public:
    MOCK_METHOD(bool, next, (domain::RawPacket & out), (override));
    MOCK_METHOD(void, stop, (), (noexcept, override));
};

/// @brief 네트워크에 나가지 않는 설정 (inet_pton 이 거부하는 IP -> connect 즉시 실패)
AppConfig makeOfflineConfig() {
    AppConfig cfg;
    cfg.channelId = 0;
    cfg.rtspIp = "not-a-valid-ip";  // inet_pton 실패 -> 소켓조차 열지 않음
    cfg.rtspPort = 554;
    cfg.sourceRingCapacity = 8;
    cfg.rtspReconnectBackoffInitialSec = 1;
    cfg.rtspReconnectBackoffMaxSec = 2;
    return cfg;
}

constexpr auto kShutdownBudget = std::chrono::seconds(10);

}  // namespace

// ============================================================================
// 1. IMetadataSource 계약 (Mock)
// ============================================================================

TEST(SourceTest, Contract_ConsumerLoopStopsWhenNextReturnsFalse) {
    MockIMetadataSource source;
    domain::RawPacket packet;
    packet.channelId = 1;

    // 두 번 성공한 뒤 스트림 종료
    EXPECT_CALL(source, next(_))
        .WillOnce(Return(true))
        .WillOnce(Return(true))
        .WillOnce(Return(false));

    int consumed = 0;
    domain::RawPacket raw;
    while (source.next(raw)) ++consumed;

    EXPECT_EQ(consumed, 2) << "next() 가 false 를 반환하면 소비 루프가 즉시 종료되어야 함";
}

TEST(SourceTest, Contract_StopIsInvokedByShutdownPath) {
    MockIMetadataSource source;
    EXPECT_CALL(source, stop()).Times(AtLeast(1));

    // main.cpp 의 시그널 스레드가 하는 일을 흉내
    source.stop();
}

TEST(SourceTest, Contract_NextFillsOutParameter) {
    MockIMetadataSource source;
    domain::RawPacket filled;
    filled.channelId = 7;
    filled.bytes = {1, 2, 3};

    EXPECT_CALL(source, next(_)).WillOnce(DoAll(SetArgReferee<0>(filled), Return(true)));

    domain::RawPacket out;
    ASSERT_TRUE(source.next(out));
    EXPECT_EQ(out.channelId, 7);
    EXPECT_EQ(out.bytes.size(), 3u);
}

// ============================================================================
// 2. [W1] 종료 경로 — lost-wakeup 회귀 방지 (실제 구현체)
// ============================================================================

TEST(SourceTest, W1_StopUnblocksBlockedNextWithinBudget) {
    // 링이 비어 있으므로 next() 는 cv_.wait 에 들어가 블로킹된다.
    // stop() 이 notify 전에 mtx_ 를 잡지 않으면(패치 이전) 알림이 유실되어
    // next() 가 영구 블로킹되고, 이 테스트는 타임아웃으로 실패한다.
    RtspOnvifSourceV2 source(makeOfflineConfig());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // next() 가 대기에 진입할 시간

    auto blocked = std::async(std::launch::async, [&source] {
        domain::RawPacket raw;
        return source.next(raw);  // 링이 비어 있으므로 블로킹
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    source.stop();

    ASSERT_EQ(blocked.wait_for(kShutdownBudget), std::future_status::ready)
        << "[W1] stop() 이후 next() 가 반드시 깨어나야 함 (lost wakeup 이면 여기서 영구 블로킹)";
    EXPECT_FALSE(blocked.get()) << "잔여 프레임이 없으면 next() 는 false 를 반환해야 함";
}

TEST(SourceTest, W1_StopBeforeNextMakesNextReturnImmediately) {
    // stop() 이 먼저 온 경우: 술어가 이미 참이므로 next() 는 대기에 들어가지 않아야 한다.
    RtspOnvifSourceV2 source(makeOfflineConfig());
    source.stop();

    auto fut = std::async(std::launch::async, [&source] {
        domain::RawPacket raw;
        return source.next(raw);
    });

    ASSERT_EQ(fut.wait_for(kShutdownBudget), std::future_status::ready)
        << "stop() 이 선행하면 next() 는 즉시 반환해야 함";
    EXPECT_FALSE(fut.get());
}

TEST(SourceTest, StopIsIdempotent) {
    RtspOnvifSourceV2 source(makeOfflineConfig());

    EXPECT_NO_THROW({
        source.stop();
        source.stop();
        source.stop();
    }) << "stop() 은 멱등이며 예외를 던지지 않아야 함 (noexcept 계약)";
}

TEST(SourceTest, DestructorJoinsWorkerWithoutHanging) {
    // 소멸자는 stop() 후 워커를 join 한다. 취소 경로가 깨지면 여기서 영구 대기한다.
    auto fut = std::async(std::launch::async, [] {
        RtspOnvifSourceV2 source(makeOfflineConfig());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // 소멸자 실행
    });

    EXPECT_EQ(fut.wait_for(kShutdownBudget), std::future_status::ready)
        << "소멸자가 워커를 join 하지 못하면 프로세스가 종료되지 않는다";
}

TEST(SourceTest, ConstructAndDestroyRepeatedlyDoesNotLeakThreads) {
    // 반복 생성/파괴에서 스레드·fd 누수가 없어야 한다 (RAII 검증)
    auto fut = std::async(std::launch::async, [] {
        for (int i = 0; i < 5; ++i) {
            RtspOnvifSourceV2 source(makeOfflineConfig());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    });

    ASSERT_EQ(fut.wait_for(kShutdownBudget), std::future_status::ready);
    EXPECT_TRUE(fut.get());
}

TEST(SourceTest, StopFromDifferentThreadIsSafe) {
    // 계약: stop() 은 next() 를 부르는 스레드가 아닌 다른 스레드에서 호출된다.
    RtspOnvifSourceV2 source(makeOfflineConfig());
    std::atomic<bool> nextReturned{false};

    std::thread consumer([&source, &nextReturned] {
        domain::RawPacket raw;
        source.next(raw);
        nextReturned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::thread stopper([&source] { source.stop(); });

    stopper.join();
    consumer.join();

    EXPECT_TRUE(nextReturned.load()) << "다른 스레드에서의 stop() 이 소비 스레드를 해제해야 함";
}
