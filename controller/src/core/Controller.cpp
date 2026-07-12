/**
 * @file    Controller.cpp
 * @brief   Controller 구현부
 * @note    와이어 포맷을 JSON으로 가정하고 nlohmann::json을 사용한다
 */
#include "core/Controller.h"

#include <chrono>
#include <nlohmann/json.hpp>

#include "model/incoming/MetadataPacket.h"

namespace {

/**
 * @brief   raw bytes(JSON)를 MetadataPacket으로 역직렬화한다.
 * @note    MetadataPacket 필드 정의와 강결합되어 있어 인터페이스로 추상화하지 않는다.
 * @param   raw     수신된 바이트 데이터
 * @param   out     역직렬화 결과가 채워짐
 * @return  성공 여부. 필수 필드가 없거나 JSON 파싱에 실패하면 false.
 */
bool deserializeMetadataPacket(const std::vector<uint8_t>& raw, MetadataPacket& out) {
    try {
        auto j = nlohmann::json::parse(raw);
        out.channelId = j.at("channelId").get<std::string>();
        out.timestamp = j.at("timestamp").get<uint64_t>();
        out.objectId = j.at("objectId").get<int>();
        out.objectType = j.at("objectType").get<std::string>();
        out.x = j.at("x").get<double>();
        out.y = j.at("y").get<double>();
        out.halfWidth = j.value("halfWidth", 0.0);
        out.halfHeight = j.value("halfHeight", 0.0);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

}  // namespace

Controller::Controller(INetwork* subscribeNetwork, IParser* parser, IRiskDetector* riskDetector, IAppSender* appSender,
                       IDriverSender* driverSender, uint32_t frameIntervalMs)
    : subscribeNetwork_(subscribeNetwork),
      parser_(parser),
      riskDetector_(riskDetector),
      appSender_(appSender),
      driverSender_(driverSender),
      frameIntervalMs_(frameIntervalMs),
      running_(false) {}

Controller::~Controller() { stop(); }

void Controller::start() {
    running_ = true;
    receiveThread_ = std::thread(&Controller::receiveLoop, this);
    timerThread_ = std::thread(&Controller::timerLoop, this);
}

void Controller::stop() {
    running_ = false;
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
}

void Controller::receiveLoop() {
    while (running_) {
        std::vector<uint8_t> raw;
        if (!subscribeNetwork_->receive(raw)) {
            continue;
        }

        MetadataPacket packet;
        if (!deserializeMetadataPacket(raw, packet)) {
            // TODO: 로깅. 역직렬화 실패한 패킷은 폐기한다.
            continue;
        }

        ParsedResult result = parser_->parse(packet);
        if (result.trackedPoint) {
            aggregator_.push(*result.trackedPoint);
        } else if (result.blurTarget) {
            appSender_->sendBlurTarget(*result.blurTarget);
        } else {
            // TODO: 로깅. trackedPoint, blurTarget 둘 다 비어있으면 파싱 실패로 간주.
        }
    }
}

void Controller::timerLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(frameIntervalMs_));

        std::vector<TrackedPoint> frame = aggregator_.collectFrame();
        RiskResult riskResult = riskDetector_->evaluate(frame);

        AppPacket appPacket;
        appPacket.points = frame;
        appPacket.channelStatuses = riskResult.channelStatuses;
        appSender_->sendTrackedFrame(appPacket);

        for (const auto& status : riskResult.channelStatuses) {
            if (status.level == DriverLevel::CLEAR) {
                continue;
            }
            DriverPacket driverPacket;
            driverPacket.channelId = status.channelId;
            driverPacket.level = status.level;
            driverSender_->sendChannelStatus(driverPacket);
        }
    }
}