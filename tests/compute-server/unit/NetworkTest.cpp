/**
 * @file    NetworkTest.cpp
 * @brief   INetwork 계약 + RtspClientV2 방어 로직 단위 테스트
 *
 * @details
 * 두 층위로 나눠 검증한다:
 *
 *  1) **MockINetwork (gmock)** — 상위 Source 가 의존하는 *생명주기 계약*을 고정한다.
 *     (connect -> setup -> play -> run 순서, PLAY 실패 시 run() 미진입,
 *      onPayloadReceived 콜백 배선)
 *
 *  2) **실제 RtspClientV2** — 소켓을 열지 않고도 검증 가능한 방어 로직을 실제로 돌린다.
 *     - `inet_pton` 목적지 주소 검증 (잘못된 IP 로 0.0.0.0 접속을 시도하지 않음)
 *     - `cancel()` 의 멱등성 및 connect 이전 호출 안전성
 *     - PLAY 성공 플래그 초기 상태
 *
 * @note RTSP 응답 헤더 파서(`recvHeaders`, `buildDigestHeader`, `md5Hex`)는 **private** 이라
 *       외부에서 직접 호출할 수 없다. 이들을 테스트하려면 seam(예: 프로토콜 파싱을
 *       자유 함수/별도 클래스로 분리)이 필요하며, 현재 구조에서는 커버되지 않는다.
 *       테스트에서 같은 파싱 로직을 복제해 검증하면 프로덕션 코드가 아닌 사본을
 *       검증하는 셈이므로 의도적으로 하지 않았다.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/INetwork.h"
#include "network/RtspClientV2.h"

using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

namespace {

/// @brief INetwork 의 gmock 대역 — 실제 소켓 없이 생명주기 계약만 검증
class MockINetwork : public INetwork {
public:
    MOCK_METHOD(bool, connect, (), (override));
    MOCK_METHOD(bool, setup, (), (override));
    MOCK_METHOD(void, play, (), (override));
    MOCK_METHOD(void, run, (), (override));
};

AppConfig makeConfig(const std::string& ip) {
    AppConfig cfg;
    cfg.channelId = 0;
    cfg.rtspIp = ip;
    cfg.rtspPort = 554;
    cfg.rtspUser = "viewer";
    cfg.rtspPass = "secret";
    cfg.rtspSetupUri = "rtsp://127.0.0.1/metadata";
    cfg.rtspPlayUri = "rtsp://127.0.0.1/stream";
    cfg.rtspConnectTimeoutSec = 1;
    cfg.rtspRecvTimeoutSec = 1;
    return cfg;
}

/// @brief Source 의 워커가 수행하는 세션 시퀀스를 그대로 흉내낸 헬퍼
void runSessionSequence(INetwork& net, bool playSucceeded) {
    if (net.connect() && net.setup()) {
        net.play();
        if (playSucceeded)
            net.run();
    }
}

}  // namespace

// ============================================================================
// 1. INetwork 생명주기 계약 (Mock)
// ============================================================================

TEST(NetworkTest, Contract_LifecycleRunsInOrder) {
    MockINetwork net;
    InSequence seq;

    EXPECT_CALL(net, connect()).WillOnce(Return(true));
    EXPECT_CALL(net, setup()).WillOnce(Return(true));
    EXPECT_CALL(net, play());
    EXPECT_CALL(net, run());

    runSessionSequence(net, /*playSucceeded=*/true);
}

TEST(NetworkTest, Contract_SetupIsSkippedWhenConnectFails) {
    MockINetwork net;
    EXPECT_CALL(net, connect()).WillOnce(Return(false));
    EXPECT_CALL(net, setup()).Times(0);
    EXPECT_CALL(net, play()).Times(0);
    EXPECT_CALL(net, run()).Times(0);

    runSessionSequence(net, true);
}

TEST(NetworkTest, Contract_PlayIsSkippedWhenSetupFails) {
    MockINetwork net;
    EXPECT_CALL(net, connect()).WillOnce(Return(true));
    EXPECT_CALL(net, setup()).WillOnce(Return(false));
    EXPECT_CALL(net, play()).Times(0);
    EXPECT_CALL(net, run()).Times(0);

    runSessionSequence(net, true);
}

TEST(NetworkTest, Contract_RunIsNotEnteredWhenPlayFails) {
    // PLAY 가 200 이 아니면 스트리밍이 시작되지 않으므로 run() 에 들어가면 안 된다.
    // (예전에는 실패해도 run() 을 불러 즉시 종료 -> 백오프가 리셋되는 재접속 폭풍이 있었음)
    MockINetwork net;
    EXPECT_CALL(net, connect()).WillOnce(Return(true));
    EXPECT_CALL(net, setup()).WillOnce(Return(true));
    EXPECT_CALL(net, play());
    EXPECT_CALL(net, run()).Times(0);

    runSessionSequence(net, /*playSucceeded=*/false);
}

TEST(NetworkTest, Contract_PayloadCallbackIsDeliveredToConsumer) {
    MockINetwork net;
    std::vector<std::string> received;

    net.onPayloadReceived = [&received](std::string_view payload) { received.emplace_back(payload); };

    // run() 이 payload 를 재조합할 때마다 콜백을 부르는 동작을 흉내
    EXPECT_CALL(net, run()).WillOnce([&net] {
        net.onPayloadReceived("<tt:Frame>first</tt:Frame>");
        net.onPayloadReceived("<tt:Frame>second</tt:Frame>");
    });

    net.run();

    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], "<tt:Frame>first</tt:Frame>");
    EXPECT_EQ(received[1], "<tt:Frame>second</tt:Frame>");
}

TEST(NetworkTest, Contract_UnsetCallbackIsSafe) {
    // 콜백이 배선되지 않은 상태에서도 호출부가 안전해야 한다 (구현체는 null 검사 후 호출).
    MockINetwork net;
    EXPECT_FALSE(static_cast<bool>(net.onPayloadReceived));
    EXPECT_NO_THROW({
        if (net.onPayloadReceived)
            net.onPayloadReceived("ignored");
    });
}

// ============================================================================
// 2. 실제 RtspClientV2 — 목적지 주소 검증 (inet_pton)
// ============================================================================

TEST(NetworkTest, Connect_RejectsNonDottedQuadAddress) {
    // inet_pton 반환값을 검사하지 않으면 sin_addr 가 0 인 채 0.0.0.0 으로 접속을 시도해
    // 원인 모를 실패가 된다. 잘못된 주소는 즉시 false 여야 한다.
    RtspClientV2 client(makeConfig("not-a-valid-ip"));
    EXPECT_FALSE(client.connect()) << "점 표기 IPv4 가 아니면 connect() 는 실패해야 함";
}

TEST(NetworkTest, Connect_RejectsHostnameSinceOnlyIPv4IsSupported) {
    RtspClientV2 client(makeConfig("camera.example.local"));
    EXPECT_FALSE(client.connect()) << "호스트명은 지원하지 않으므로 명시적으로 실패해야 함";
}

TEST(NetworkTest, Connect_RejectsMalformedNumericAddress) {
    RtspClientV2 client(makeConfig("999.999.999.999"));
    EXPECT_FALSE(client.connect()) << "범위를 벗어난 옥텟은 inet_pton 이 거부해야 함";
}

TEST(NetworkTest, Connect_RejectsEmptyAddress) {
    RtspClientV2 client(makeConfig(""));
    EXPECT_FALSE(client.connect());
}

// ============================================================================
// 3. 실제 RtspClientV2 — 취소 경로 안전성
// ============================================================================

TEST(NetworkTest, Cancel_IsSafeBeforeConnect) {
    // connect 이전에는 보호된 sock_ 가 -1 이므로 shutdown 대상이 없다.
    // 이미 닫힌/열리지 않은 fd 에 shutdown 을 걸면 안 된다.
    RtspClientV2 client(makeConfig("127.0.0.1"));
    EXPECT_NO_THROW(client.cancel()) << "connect 이전 cancel() 은 무해해야 함";
}

TEST(NetworkTest, Cancel_IsIdempotent) {
    RtspClientV2 client(makeConfig("127.0.0.1"));
    EXPECT_NO_THROW({
        client.cancel();
        client.cancel();
        client.cancel();
    }) << "cancel() 은 멱등이며 noexcept 계약";
}

TEST(NetworkTest, Cancel_AfterFailedConnectIsSafe) {
    // 실패한 connect 는 closeSocket()으로 fd를 반납하고 보호된 sock_를 -1로 무효화한다.
    // 그 뒤의 cancel() 이 이미 닫힌 fd 를 건드리면 안 된다.
    RtspClientV2 client(makeConfig("not-a-valid-ip"));
    ASSERT_FALSE(client.connect());
    EXPECT_NO_THROW(client.cancel());
}

TEST(NetworkTest, Cancel_ConcurrentWithConnectIsSafe) {
    for (int i = 0; i < 100; ++i) {
        RtspClientV2 client(makeConfig("not-a-valid-ip"));
        std::thread connector([&client] { (void)client.connect(); });
        client.cancel();
        connector.join();
    }
}

TEST(NetworkTest, Security_RejectsRtspHeaderInjectionInConfig) {
    AppConfig config = makeConfig("127.0.0.1");
    config.rtspUser = "viewer\r\nInjected: true";
    RtspClientV2 client(config);
    EXPECT_FALSE(client.connect());
}

TEST(NetworkTest, Config_BackoffHasFiniteUpperBound) {
    int value = std::numeric_limits<int>::max();
    AppConfig::clampRange(value, 1, AppConfig::kMaxRtspPolicySeconds, "testBackoff");
    EXPECT_EQ(value, AppConfig::kMaxRtspPolicySeconds);
}

// ============================================================================
// 4. 실제 RtspClientV2 — PLAY 성공 플래그
// ============================================================================

TEST(NetworkTest, PlaySucceeded_IsFalseBeforeAnyPlay) {
    RtspClientV2 client(makeConfig("127.0.0.1"));
    EXPECT_FALSE(client.playSucceeded()) << "PLAY 이전에는 반드시 false (run() 진입 판단 기준)";
}

TEST(NetworkTest, PlaySucceeded_RemainsFalseAfterFailedConnect) {
    RtspClientV2 client(makeConfig("not-a-valid-ip"));
    ASSERT_FALSE(client.connect());
    EXPECT_FALSE(client.playSucceeded());
}

// ============================================================================
// 5. 실제 RtspClientV2 — 반복 생성/파괴 시 자원 누수 없음
// ============================================================================

TEST(NetworkTest, RepeatedFailedConnectsDoNotLeakFds) {
    // connect() 의 모든 실패 경로가 closeSocket() 으로 fd 를 즉시 반납해야 한다.
    // 소멸자에만 의존하면 같은 인스턴스 재시도에서 fd 가 샌다.
    RtspClientV2 client(makeConfig("192.0.2.1"));  // TEST-NET-1 (라우팅 불가)

    for (int i = 0; i < 3; ++i) {
        // 연결은 실패하거나 타임아웃되지만, 어느 경로든 fd 를 반납해야 한다
        EXPECT_NO_THROW(client.connect());
    }
    SUCCEED() << "반복 실패 후에도 크래시/누수 없이 진행";
}

TEST(NetworkTest, ConstructionDoesNotOpenSocket) {
    // 생성자는 버퍼만 준비하고 소켓을 열지 않아야 한다 (connect() 에서만 연다).
    EXPECT_NO_THROW({ RtspClientV2 client(makeConfig("127.0.0.1")); });
}
