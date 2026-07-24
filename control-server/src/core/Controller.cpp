#include "core/Controller.h"

#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Controller";
}  // namespace

Controller::Controller(std::shared_ptr<IChannelReceiver> receiver, std::shared_ptr<IFrameAggregator> aggregator,
                       std::shared_ptr<ILocalToWorldTransform> transform, std::shared_ptr<ICrossChannelFuser> fuser,
                       std::shared_ptr<IZoneMapper> zoneMapper, std::shared_ptr<IRiskPolicy> riskPolicy,
                       std::shared_ptr<IHwEventDispatcher> dispatcher, std::shared_ptr<ISink> sink,
                       std::shared_ptr<IClock> clock, int channelCount)
    : receiver_(std::move(receiver)),
      aggregator_(std::move(aggregator)),
      transform_(std::move(transform)),
      fuser_(std::move(fuser)),
      zoneMapper_(std::move(zoneMapper)),
      riskPolicy_(std::move(riskPolicy)),
      dispatcher_(std::move(dispatcher)),
      sink_(std::move(sink)),
      clock_(std::move(clock)),
      channelCount_(channelCount > 0 ? channelCount : 1),
      channelAlive_(static_cast<std::size_t>(channelCount_), false),
      hardwareAlive_(static_cast<std::size_t>(channelCount_), false),
      indicatorState_(static_cast<std::size_t>(channelCount_)) {
    receiver_->setCallback([this](const veda::TopViewFrame& frame) { aggregator_->push(frame); });

    // 채널 생존 신호(LWT) 배선 -- 구독만 해놓고 버려지던 경로를 여기서 받는다
    receiver_->setAliveCallback(
        [this](veda::ChannelId channel, bool alive) { this->onChannelAlive(channel, alive, "MQTT"); });

    // STM32 하트비트 + 표시 상태(상행) 배선
    dispatcher_->setStatusCallback([this](veda::ChannelId channel, bool alive, const HwIndicatorState& indicators) {
        this->onHardwareStatus(channel, alive, indicators);
    });

    aggregator_->setCallback(
        [this](std::vector<veda::TopViewFrame> frames) { this->processPipeline(std::move(frames)); });
}

veda::ChannelStatus Controller::buildStatusLocked(std::size_t idx) const {
    // cameraAlive/hardwareAlive/표시 상태 중 하나만 바뀌어도 최신 조합 전체를 다시 보냄
    // (Qt 쪽이 부분 갱신이 아니라 항상 최신 스냅샷으로 받도록). ts 는 호출자가 락 밖에서 채움.
    veda::ChannelStatus status;
    status.ch = static_cast<veda::ChannelId>(idx);
    status.cameraAlive = channelAlive_[idx];
    status.hardwareAlive = hardwareAlive_[idx];
    status.sirenOn = indicatorState_[idx].sirenOn;
    status.buzzerOn = indicatorState_[idx].buzzerOn;
    status.ledRed = indicatorState_[idx].ledRed;
    status.ledYellow = indicatorState_[idx].ledYellow;
    status.ledGreen = indicatorState_[idx].ledGreen;
    return status;
}

void Controller::onChannelAlive(veda::ChannelId channel, bool alive, const char* source) {
    if (channel < 0 || channel >= channelCount_) {
        logError(kIface, std::string(source) + " 생존 신호의 채널 " + std::to_string(channel) + " 이 범위 밖 - 무시");
        return;
    }

    const std::size_t idx = static_cast<std::size_t>(channel);
    bool changed = false;
    veda::ChannelStatus status;
    {
        std::lock_guard<std::mutex> lock(aliveMutex_);
        changed = channelAlive_[idx] != alive;
        channelAlive_[idx] = alive;
        status = buildStatusLocked(idx);
    }  // <- sink 발행(MQTT publish, 블로킹 가능)은 락 밖에서

    // 전환될 때만 로그+발행 -- retained 메시지나 반복 신호가 와도 도배되지 않음
    if (changed) {
        const std::string message =
            std::string(source) + " 채널 " + std::to_string(channel) + (alive ? " 복구됨(alive)" : " 끊김(dead)");
        if (alive)
            logSuccess(kIface, message);
        else
            logError(kIface, message);

        status.ts = clock_ ? clock_->now() : 0;
        sink_->sendChannelStatus(status);
    }
}

/**
 * @details STM32 하트비트(alive)와 실제 경광등/부저/LED 표시 상태(indicators)를 함께 받아
 *          저장하고, 둘 중 하나라도 바뀌면 cameraAlive 와 합쳐 발행한다.
 *          IHwEventDispatcher::StatusCallback의 계약대로, 이 함수는 alive 나 indicators
 *          가 실제로 바뀌었을 때만 호출된다 (dedup은 SerialHwEventDispatcher 쪽에서 이미 함).
 */
void Controller::onHardwareStatus(veda::ChannelId channel, bool alive, const HwIndicatorState& indicators) {
    if (channel < 0 || channel >= channelCount_) {
        logError(kIface, "STM32 상태 보고의 채널 " + std::to_string(channel) + " 이 범위 밖 - 무시");
        return;
    }

    const std::size_t idx = static_cast<std::size_t>(channel);
    veda::ChannelStatus status;
    {
        std::lock_guard<std::mutex> lock(aliveMutex_);
        hardwareAlive_[idx] = alive;
        indicatorState_[idx] = indicators;
        status = buildStatusLocked(idx);
    }  // <- sink 발행(MQTT publish, 블로킹 가능)은 락 밖에서

    const std::string message =
        std::string("STM32 채널 ") + std::to_string(channel) + (alive ? " 상태 갱신(alive)" : " 끊김(dead)");
    if (alive)
        logSuccess(kIface, message);
    else
        logError(kIface, message);

    status.ts = clock_ ? clock_->now() : 0;
    sink_->sendChannelStatus(status);
}

Controller::~Controller() { stop(); }

void Controller::start() { receiver_->start(); }

void Controller::stop() { receiver_->stop(); }

void Controller::processPipeline(std::vector<veda::TopViewFrame> frames) {
    if (frames.empty()) {
        return;
    }

    // 로컬(veda::TopViewFrame) -> 월드(domain::ObservationFrame). 타입이 달라지므로 이후 단계에서
    // 로컬 좌표를 실수로 쓰면 컴파일이 실패한다 (예전에는 같은 버퍼를 in-place 로 덮어썼음)
    transform_->transform(frames, observations_);

    auto worldFrame = fuser_->fuse(observations_);

    zoneMapper_->assign(worldFrame);

    auto riskEval = riskPolicy_->evaluate(worldFrame);

    dispatcher_->dispatch(riskEval);

    sink_->send(worldFrame);

    // 윈도우마다(기본 100ms = 초당 10회) 도는 정상 경로라 Debug
    // -- 문자열 조립까지 레벨로 걸러냄
    if (isLogEnabled(LogLevel::Debug)) {
        veda::RiskLevel maxLevel = veda::RiskLevel::None;
        for (const auto& zone : riskEval.zoneLevels) {
            if (zone.level > maxLevel)
                maxLevel = zone.level;
        }
        logDebug(kIface, std::to_string(frames.size()) + "채널 → 객체 " + std::to_string(worldFrame.objects.size()) +
                             "개 융합, 최고 위험도=" + std::string(veda::toString(maxLevel)));
    }
}