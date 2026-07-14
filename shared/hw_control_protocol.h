#pragma once

/**
 * @file HwControlProtocol.h
 * @brief control-server 와 HW 제어 모듈 간의 MQTT 통신 규약
 */

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace veda {
namespace hw {

/**
 * @brief 개별 채널의 액추에이터 목표 상태
 */
struct HwDeviceState {
    bool siren = false;
    bool buzzer = false;
    bool ledRed = false;
    bool ledYellow = false;
    bool ledGreen = false;
};

/**
 * @brief HW 제어 명령 메시지
 */
struct HwControlCmd {
    int v = 1;
    int64_t ts = 0;       // 명령 발생 시각
    int channelId = 0;    // 대상 채널
    HwDeviceState state;  // 목표 상태
};

namespace topic {
// MQTT Topic: veda/hw/cmd (QoS 1)
inline constexpr auto kHwCmd = "veda/hw/cmd";
}  // namespace topic

// JSON 직렬화/역직렬화 (Contract.h의 detail namespace 사용 권장)
inline void to_json(nlohmann::json& j, const HwDeviceState& s) {
    j = nlohmann::json{{"siren", s.siren},
                       {"buzzer", s.buzzer},
                       {"ledRed", s.ledRed},
                       {"ledYellow", s.ledYellow},
                       {"ledGreen", s.ledGreen}};
}
inline void from_json(const nlohmann::json& j, HwDeviceState& s) {
    s.siren = j.value("siren", false);
    s.buzzer = j.value("buzzer", false);
    s.ledRed = j.value("ledRed", false);
    s.ledYellow = j.value("ledYellow", false);
    s.ledGreen = j.value("ledGreen", false);
}

inline void to_json(nlohmann::json& j, const HwControlCmd& c) {
    j = nlohmann::json{{"v", c.v}, {"ts", c.ts}, {"channelId", c.channelId}, {"state", c.state}};
}
inline void from_json(const nlohmann::json& j, HwControlCmd& c) {
    c.v = j.value("v", 1);
    c.ts = j.value("ts", 0LL);
    c.channelId = j.value("channelId", 0);
    if (j.contains("state")) {
        j.at("state").get_to(c.state);
    }
}

}  // namespace hw
}  // namespace veda