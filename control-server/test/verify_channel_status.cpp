/**
 * @file    verify_channel_status.cpp
 * @brief   채널 하드웨어/연결 상태(veda::ChannelStatus)가 Qt 클라이언트까지 배선되는지 검증
 *
 * @details
 * 실제 Controller.cpp 를 페이크 의존성(IChannelReceiver/IHwEventDispatcher/ISink/IClock 등)과
 * 함께 링크해서 돌린다. MQTT/UART 없이 콜백 배선만 검증하므로 libmosquitto 의존이 없다.
 *
 * @par [ 빌드 & 실행 ]
 * @code
 * cmake -B build && cmake --build build --target verify-channel-status
 * ./build/verify-channel-status
 * @endcode
 *
 * @note [ 이 테스트가 고정하려는 회귀 ]
 * 예전에는 MqttChannelReceiver 가 veda/ch/+/alive 를 구독해놓고도, 그리고
 * SerialHwEventDispatcher 가 STM32 하트비트를 정상 수신해도, Controller::onChannelAlive() 가
 * 내부 상태만 갱신하고 로그만 남길 뿐 Qt 클라이언트로는 아무것도 나가지 않았음
 * (wire contract에 채널 상태 메시지 자체가 없었음)
 * -> veda::ChannelStatus + ISink::sendChannelStatus() 로 배선한 뒤 이 경로를 고정
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/Controller.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

void section(const std::string& title) { std::printf("\n=== %s ===\n", title.c_str()); }

/// @brief 테스트 대상이 남기는 로그를 삼켜 결과 출력이 묻히지 않게 함
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

// ---------------------------------------------------------------------------
// Controller 생성자가 즉시 호출하는 setCallback/setAliveCallback/setStatusCallback 만
// 실제로 동작하면 되므로, 나머지 메서드는 이 테스트에서 호출되지 않는 한 비어 있어도 됨
// (processPipeline 을 트리거하지 않으므로 fuser/zoneMapper/riskPolicy 는 손대지 않음)
// ---------------------------------------------------------------------------

class FakeReceiver final : public IChannelReceiver {
public:
    void setCallback(FrameCallback) override {}
    void setAliveCallback(AliveCallback callback) override { aliveCallback = std::move(callback); }
    void start() override {}
    void stop() override {}

    AliveCallback aliveCallback;
};

class FakeAggregator final : public IFrameAggregator {
public:
    void setCallback(AggregationCallback) override {}
    void push(const veda::TopViewFrame&) override {}
};

class FakeTransform final : public ILocalToWorldTransform {
public:
    void transform(const std::vector<veda::TopViewFrame>&, std::vector<domain::ObservationFrame>& out) override {
        out.clear();
    }
};

class FakeFuser final : public ICrossChannelFuser {
public:
    domain::WorldFrame fuse(const std::vector<domain::ObservationFrame>&) override { return {}; }
};

class FakeZoneMapper final : public IZoneMapper {
public:
    void assign(domain::WorldFrame&) override {}
};

class FakeRiskPolicy final : public IRiskPolicy {
public:
    domain::RiskEvaluation evaluate(domain::WorldFrame&) override { return {}; }
};

class FakeDispatcher final : public IHwEventDispatcher {
public:
    void dispatch(const domain::RiskEvaluation&) override {}
    void setStatusCallback(StatusCallback callback) override { statusCallback = std::move(callback); }
    void setFaultCallback(FaultCallback) override {}

    StatusCallback statusCallback;
};

/// @brief sendChannelStatus 호출을 기록하는 페이크 sink -- Qt 로 실제 나갈 메시지를 대신 검사
class RecordingSink final : public ISink {
public:
    void send(const domain::WorldFrame&) override {}
    void sendChannelStatus(const veda::ChannelStatus& status) override { calls.push_back(status); }

    std::vector<veda::ChannelStatus> calls;
};

class FakeClock final : public IClock {
public:
    veda::TimestampMs now() const override { return tick; }
    veda::TimestampMs tick = 1000;
};

}  // namespace

int main() {
    LogSilencer silencer;

    auto receiver = std::make_shared<FakeReceiver>();
    auto aggregator = std::make_shared<FakeAggregator>();
    auto transform = std::make_shared<FakeTransform>();
    auto fuser = std::make_shared<FakeFuser>();
    auto zoneMapper = std::make_shared<FakeZoneMapper>();
    auto riskPolicy = std::make_shared<FakeRiskPolicy>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    auto sink = std::make_shared<RecordingSink>();
    auto clock = std::make_shared<FakeClock>();

    Controller controller(receiver, aggregator, transform, fuser, zoneMapper, riskPolicy, dispatcher, sink, clock,
                          /*channelCount=*/4);

    check(static_cast<bool>(receiver->aliveCallback), "Controller 생성자가 receiver->setAliveCallback 을 등록함");
    check(static_cast<bool>(dispatcher->statusCallback), "Controller 생성자가 dispatcher->setStatusCallback 을 등록함");

    // -----------------------------------------------------------------------
    section("1) MQTT LWT(카메라) 전환이 Qt로 발행됨");
    // -----------------------------------------------------------------------
    {
        receiver->aliveCallback(0, true);
        check(sink->calls.size() == 1, "채널 0 alive=true 전환 시 sendChannelStatus 1회 호출");
        if (!sink->calls.empty()) {
            const auto& s = sink->calls.back();
            check(s.ch == 0, "  └ ch=0");
            check(s.cameraAlive == true, "  └ cameraAlive=true");
            check(s.hardwareAlive == false, "  └ hardwareAlive는 아직 false (STM32 신호 없음)");
            check(s.ts == 1000, "  └ ts는 주입한 IClock 값 사용");
        }
    }

    // -----------------------------------------------------------------------
    section("2) 같은 값 반복 수신은 재발행하지 않음 (retained 폭주 방지)");
    // -----------------------------------------------------------------------
    {
        const std::size_t before = sink->calls.size();
        receiver->aliveCallback(0, true);  // 이미 true -> 전환 아님
        receiver->aliveCallback(0, true);
        check(sink->calls.size() == before, "동일 값 반복 수신은 발행하지 않음 (dedup)");
    }

    // -----------------------------------------------------------------------
    section("3) STM32 하트비트 전환도 같은 경로로 발행되고, 세 상태가 합쳐짐");
    // -----------------------------------------------------------------------
    {
        clock->tick = 2000;
        const HwIndicatorState indicators{/*sirenOn=*/true, /*buzzerOn=*/false, /*ledRed=*/true,
                                          /*ledYellow=*/false, /*ledGreen=*/false};
        dispatcher->statusCallback(0, true, indicators);
        check(sink->calls.size() == 2, "STM32 alive=true 전환도 발행됨");
        const auto& s = sink->calls.back();
        check(s.cameraAlive == true, "  └ cameraAlive는 이전 상태(true) 유지");
        check(s.hardwareAlive == true, "  └ hardwareAlive=true 로 갱신");
        check(s.sirenOn == true && s.ledRed == true && s.buzzerOn == false && s.ledYellow == false &&
                  s.ledGreen == false,
              "  └ 경광등/부저/LED 표시 상태가 그대로 실림");
        check(s.ts == 2000, "  └ ts가 최신 IClock 값으로 갱신됨");
    }

    // -----------------------------------------------------------------------
    section("4) 카메라만 끊겨도 STM32 상태와 합쳐서 재발행");
    // -----------------------------------------------------------------------
    {
        receiver->aliveCallback(0, false);
        const auto& s = sink->calls.back();
        check(s.cameraAlive == false, "cameraAlive=false 로 전환");
        check(s.hardwareAlive == true, "hardwareAlive는 true 유지 (STM32는 별개로 살아있음)");
        check(s.sirenOn == true && s.ledRed == true, "  └ 표시 상태는 카메라 전환과 무관하게 유지됨");
    }

    // -----------------------------------------------------------------------
    section("5) 채널이 다르면 서로 간섭하지 않음");
    // -----------------------------------------------------------------------
    {
        const std::size_t before = sink->calls.size();
        receiver->aliveCallback(1, true);
        check(sink->calls.size() == before + 1, "채널 1 전환이 별도로 발행됨");
        check(sink->calls.back().ch == 1, "  └ ch=1");
        check(sink->calls.back().cameraAlive == true, "  └ ch=1 cameraAlive=true");
        check(sink->calls.back().hardwareAlive == false, "  └ ch=1 hardwareAlive는 ch=0과 독립적으로 false");
        check(sink->calls.back().sirenOn == false, "  └ ch=1 표시 상태는 ch=0과 독립적으로 기본값(false)");
    }

    // -----------------------------------------------------------------------
    section("6) 범위 밖 채널은 무시하고 발행하지 않음");
    // -----------------------------------------------------------------------
    {
        const std::size_t before = sink->calls.size();
        receiver->aliveCallback(99, true);
        check(sink->calls.size() == before, "channelCount(4) 밖의 채널은 발행되지 않음");
    }

    // -----------------------------------------------------------------------
    section("7) veda::ChannelStatus JSON 왕복 + 토픽 형식");
    // -----------------------------------------------------------------------
    {
        veda::ChannelStatus original;
        original.ch = 2;
        original.ts = 1234567890;
        original.cameraAlive = true;
        original.hardwareAlive = false;
        original.sirenOn = true;
        original.buzzerOn = true;
        original.ledRed = false;
        original.ledYellow = true;
        original.ledGreen = false;

        const std::string payload = veda::encode(original);
        const veda::ChannelStatus decoded = veda::decode<veda::ChannelStatus>(payload);
        check(decoded.v == veda::kSchemaVersion, "디코드된 v가 현재 스키마 버전과 일치");
        check(decoded.ch == 2 && decoded.ts == 1234567890 && decoded.cameraAlive == true &&
                  decoded.hardwareAlive == false,
              "JSON 왕복 후 필드가 그대로 보존됨");
        check(decoded.sirenOn == true && decoded.buzzerOn == true && decoded.ledRed == false &&
                  decoded.ledYellow == true && decoded.ledGreen == false,
              "  └ siren/buzzer/led 필드도 JSON 왕복 후 그대로 보존됨");

        check(veda::topic::hwStatus(2) == "veda/hw/ch/2/status", "토픽 형식이 veda/hw/ch/{ch}/status");
        check(std::string(veda::topic::kHwStatusAll) == "veda/hw/ch/+/status", "와일드카드 토픽 형식");
        check(veda::qos::kHwStatus == 1, "QoS는 retained 메시지에 맞게 1");

        // 손상된 페이로드는 예외 없이 v=0 인 기본 객체로 (Contract.h의 decode<T>() 계약)
        const veda::ChannelStatus broken = veda::decode<veda::ChannelStatus>("{not json");
        check(broken.v == 0, "파싱 실패 시 v=0 인 기본값 반환 (예외 없음)");
    }

    section("결과");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
