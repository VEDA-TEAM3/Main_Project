#pragma once

/**
 * @file    AppConfig.h
 * @brief   관제 서버의 전체 구동 설정값을 담는 구조체
 */

#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "Contract.h"

/**
 * @brief   위험도 판정 관련 거리 임계값
 * @note    dedupMergeDistance 는 반드시 warningDistance보다 작아야 함
 *          역전되면 정상 Danger 케이스가 채널 간 dedup 병합으로 삼켜질 수 있음
 */
struct RiskConfig {
    double warningDistance = 5.0;     ///< m, 차량 기준 Warning 트리거 거리
    double dangerousDistance = 2.0;   ///< m, 차량 기준 Danger 트리거 거리
    double dedupMergeDistance = 1.0;  ///< m, 채널 간 dedup 병합 판정 거리

    /**
     * @brief   사람이 감지된 것만으로 해당 zone 을 Warning 으로 올릴지 여부
     * @details Contract.h 의 RiskLevel 정의(`Warning = 사람 감지 or 거리 이내`)에 맞춘 동작
     *          예전 구현은 차량이 있을 때만 평가해서 사람만 있으면 항상 None 이었음
     */
    bool warnOnHumanPresence = true;

    /**
     * @brief   같은 실체를 프레임 간에 이어붙일 최대 이동 거리 (m)
     * @details 이 거리 안에서 같은 클래스면 이전 프레임의 GlobalId 를 물려받음
     *          0 이하면 추적을 끄고 매 프레임 새 gid 를 부여 (예전 동작)
     */
    double trackMaxDistance = 2.0;
};

inline void from_json(const nlohmann::json& j, RiskConfig& r) {
    r.warningDistance = veda::detail::get_or<double>(j, "warningDistance", r.warningDistance);
    r.dangerousDistance = veda::detail::get_or<double>(j, "dangerousDistance", r.dangerousDistance);
    r.dedupMergeDistance = veda::detail::get_or<double>(j, "dedupMergeDistance", r.dedupMergeDistance);
    r.warnOnHumanPresence = veda::detail::get_or<bool>(j, "warnOnHumanPresence", r.warnOnHumanPresence);
    r.trackMaxDistance = veda::detail::get_or<double>(j, "trackMaxDistance", r.trackMaxDistance);
}

/**
 * @brief   채널별 zone 배정 각도 구간 (atan2 기반, 원점 = 사거리 중심)
 * @note    전체 zoneBoundaries 의 각도 구간 합은 360도여야 함 (등분 아님 — 실측값)
 */
struct ZoneBoundary {
    veda::ChannelId channelId = -1;
    double angleMinDeg = 0.0;
    double angleMaxDeg = 0.0;
};

inline void from_json(const nlohmann::json& j, ZoneBoundary& z) {
    z.channelId = veda::detail::get_or<veda::ChannelId>(j, "channelId", -1);
    z.angleMinDeg = veda::detail::get_or<double>(j, "angleMinDeg", 0.0);
    z.angleMaxDeg = veda::detail::get_or<double>(j, "angleMaxDeg", 0.0);
}

/**
 * @brief STM32 하트비트 및 명령-상태 불일치 대응 정책 (§3-C, §3-B)
 */
struct HwHealthCheckConfig {
    uint32_t heartbeatIntervalMs = 500;        ///< STM32 → RPi 상태 보고 주기
    uint32_t missedBeatsForTimeout = 3;        ///< 연속 유실 시 채널 dead 판정 기준
    uint32_t mismatchRetryCount = 2;           ///< 명령-실제상태 불일치 시 재전송 횟수
    bool mismatchEscalateAfterRetries = true;  ///< 재시도 소진 시 대시보드 fault 표시 여부

    /**
     * @brief   하드웨어 이벤트 디스패처 종류
     * @details
     * - "serial"  : SerialHwEventDispatcher — 실제 STM32 UART 링크 (기본값)
     * - "console" : ConsoleDispatcher — 하드웨어 없이 콘솔로만 확인 (개발/테스트용)
     *
     * @note 예전에는 AppContext 가 ConsoleDispatcher 를 하드코딩해서 연결하고 있었고,
     *       SerialHwEventDispatcher.cpp 는 CMakeLists 에도 없어 빌드조차 되지 않았음
     *       -> LED/사이렌/부저가 전혀 동작하지 않는 상태였으므로 기본값을 serial 로 둠
     */
    std::string dispatcher = "serial";

    /// @brief STM32 가 연결된 시리얼 장치 경로 (dispatcher == "serial" 일 때)
    std::string devicePath = "/dev/serial0";
};

inline void from_json(const nlohmann::json& j, HwHealthCheckConfig& h) {
    h.heartbeatIntervalMs = veda::detail::get_or<uint32_t>(j, "heartbeatIntervalMs", h.heartbeatIntervalMs);
    h.missedBeatsForTimeout = veda::detail::get_or<uint32_t>(j, "missedBeatsForTimeout", h.missedBeatsForTimeout);
    h.mismatchRetryCount = veda::detail::get_or<uint32_t>(j, "mismatchRetryCount", h.mismatchRetryCount);
    h.mismatchEscalateAfterRetries =
        veda::detail::get_or<bool>(j, "mismatchEscalateAfterRetries", h.mismatchEscalateAfterRetries);
    h.dispatcher = veda::detail::get_or<std::string>(j, "dispatcher", h.dispatcher);
    h.devicePath = veda::detail::get_or<std::string>(j, "devicePath", h.devicePath);
    if (h.dispatcher != "serial" && h.dispatcher != "console") {
        std::cerr << "[Config] 경고: hwHealthCheck.dispatcher=\"" << h.dispatcher
                  << "\" 는 알 수 없는 값입니다 (serial|console) — \"serial\"로 처리합니다.\n";
        h.dispatcher = "serial";
    }
}

/**
 * @brief NullReceiver 전용 파라미터
 * @note  MqttChannelReceiver로 교체되면 이 구조체 삭제
 */
struct SimulationConfig {
    uint64_t baseIntervalMs = 20;  ///< 채널당 기본 프레임 발행 주기 (ms)
    uint64_t jitterStepMs = 3;     ///< 채널 인덱스에 곱해지는 지터 오프셋 (ms)
};

inline void from_json(const nlohmann::json& j, SimulationConfig& s) {
    s.baseIntervalMs = veda::detail::get_or<uint64_t>(j, "baseIntervalMs", s.baseIntervalMs);
    s.jitterStepMs = veda::detail::get_or<uint64_t>(j, "jitterStepMs", s.jitterStepMs);
}

/**
 * @brief 카메라 로컬 좌표 → 도면 공통 월드 좌표 변환 파라미터 (채널별 1개)
 *
 * @details
 * facingAngleDeg:  나침반 규약 — 북(도면 위쪽)=0도, 시계 방향(오른쪽) 증가
 *                  atan2 규약(zoneBoundaries 등)으로는 normalize(90 - facingAngleDeg)로 변환
 * lateralSign:     로컬 +x(좌우 오프셋)가 카메라 전방 기준 (오른쪽이면 +1, 왼쪽이면 -1)
 */
struct CameraCalibration {
    veda::ChannelId channelId = -1;
    double cameraPosX = 0.0;  ///< 카메라 설치 위치, 도면 공통 좌표계(사거리 중심 원점) 기준
    double cameraPosY = 0.0;
    double facingAngleDeg = 0.0;
    int lateralSign = -1;
};

inline void from_json(const nlohmann::json& j, CameraCalibration& c) {
    c.channelId = veda::detail::get_or<veda::ChannelId>(j, "channelId", -1);
    c.cameraPosX = veda::detail::get_or<double>(j, "cameraPosX", 0.0);
    c.cameraPosY = veda::detail::get_or<double>(j, "cameraPosY", 0.0);
    c.facingAngleDeg = veda::detail::get_or<double>(j, "facingAngleDeg", 0.0);
    c.lateralSign = veda::detail::get_or<int>(j, "lateralSign", -1);
}

struct AppConfig {
    // [파이프라인 설정]
    uint64_t windowSizeMs = 100;  ///< 프레임 집계 시간 윈도우 (ms)
    int channelCount = 4;         ///< 채널(zone) 개수

    /**
     * @name 로깅 (shared/Logger.h)
     * @details compute-server 와 동일한 키/의미를 사용함
     *          프레임마다 도는 이벤트는 Debug 레벨이라 기본값(info)에서는 큐에도 안 들어감
     * @{
     */
    std::string logLevel = "info";  ///< "debug" | "info" | "error" | "off"
    bool logToConsole = true;
    bool logToFile = true;
    int logFlushIntervalMs = 500;
    int logMaxPendingEntries = 10000;
    /** @} */

    // [위험도 정책 설정]
    RiskConfig risk;

    // [zone 배정 설정]
    std::vector<ZoneBoundary> zoneBoundaries;

    // [좌표 변환 설정 — 채널별 카메라 캘리브레이션]
    std::vector<CameraCalibration> cameraCalibrations;

    // [HW 헬스체크 설정]
    HwHealthCheckConfig hwHealthCheck;

    // [테스트 시뮬레이션 설정 — NullReceiver 전용]
    SimulationConfig simulation;

    // [네트워크 설정]
    std::string mqttBrokerUrl = "tcp://localhost:1883";
    /**
     * @brief   RiskFrame 발행 토픽
     * @warning 기본값이 계약(veda::topic::kRisk = "veda/risk")과 달라서, 이대로 두면
     *          클라이언트가 구독하는 토픽으로 나가지 않음 -> 계약값으로 맞춤
     *          다른 값을 넣으면 Contract.h 를 어기는 것이므로 클라이언트도 함께 바꿔야 함
     */
    std::string mqttSendTopic = veda::topic::kRisk;
    std::string mqttCaFile = "/etc/veda/certs/ca.crt";  ///< mqttBrokerUrl이 ssl/mqtts일 때 사용하는 TLS CA 인증서 경로
    std::string mqttClientId;                           ///< 비어있으면 MqttTransport가 자동 생성
    int mqttKeepAliveSeconds = 60;
    int mqttReconnectDelaySeconds = 1;      ///< 재연결 대기 시간 초기값 (초)
    int mqttReconnectDelayMaxSeconds = 10;  ///< 재연결 대기 시간 상한 (초, 지수 백오프)

    /// @brief MqttChannelReceiver가 최초 구독/연결에 실패했을 때 재시도하는 간격 (ms)
    uint64_t mqttReceiverRetryIntervalMs = 2000;

    /**
     * @brief   외부 JSON 설정 파일에서 설정값을 읽어옵니다.
     * @details 파일이 없거나 파싱 실패 시 기본값으로 계속 진행 (예외를 던지지 않음)
     * @param   configPath 설정 파일 경로
     * @return  AppConfig 파싱된 설정값 객체
     */
    static inline AppConfig load(const std::string& configPath) {
        AppConfig config;

        std::ifstream file(configPath);
        if (!file.is_open()) {
            std::cerr << "[Config] 설정 파일을 열 수 없습니다: " << configPath << " — 기본값을 사용합니다.\n";
            return config;
        }

        nlohmann::json j;
        try {
            file >> j;
        } catch (const std::exception& e) {
            std::cerr << "[Config] JSON 파싱 실패: " << configPath << " (" << e.what() << ") — 기본값을 사용합니다.\n";
            return config;
        }

        config.windowSizeMs = veda::detail::get_or<uint64_t>(j, "windowSizeMs", config.windowSizeMs);
        config.channelCount = veda::detail::get_or<int>(j, "channelCount", config.channelCount);

        config.logLevel = veda::detail::get_or<std::string>(j, "logLevel", config.logLevel);
        if (config.logLevel != "debug" && config.logLevel != "info" && config.logLevel != "error" &&
            config.logLevel != "off") {
            std::cerr << "[Config] 경고: logLevel=\"" << config.logLevel
                      << "\" 는 알 수 없는 값입니다 (debug|info|error|off) — \"info\"로 처리합니다.\n";
            config.logLevel = "info";
        }
        config.logToConsole = veda::detail::get_or<bool>(j, "logToConsole", config.logToConsole);
        config.logToFile = veda::detail::get_or<bool>(j, "logToFile", config.logToFile);
        config.logFlushIntervalMs = veda::detail::get_or<int>(j, "logFlushIntervalMs", config.logFlushIntervalMs);
        config.logMaxPendingEntries = veda::detail::get_or<int>(j, "logMaxPendingEntries", config.logMaxPendingEntries);
        config.risk = veda::detail::get_or<RiskConfig>(j, "risk", config.risk);
        config.zoneBoundaries =
            veda::detail::get_or<std::vector<ZoneBoundary>>(j, "zoneBoundaries", config.zoneBoundaries);
        config.cameraCalibrations =
            veda::detail::get_or<std::vector<CameraCalibration>>(j, "cameraCalibrations", config.cameraCalibrations);
        config.hwHealthCheck = veda::detail::get_or<HwHealthCheckConfig>(j, "hwHealthCheck", config.hwHealthCheck);
        config.simulation = veda::detail::get_or<SimulationConfig>(j, "simulation", config.simulation);
        config.mqttBrokerUrl = veda::detail::get_or<std::string>(j, "mqttBrokerUrl", config.mqttBrokerUrl);
        config.mqttSendTopic = veda::detail::get_or<std::string>(j, "mqttSendTopic", config.mqttSendTopic);
        config.mqttCaFile = veda::detail::get_or<std::string>(j, "mqttCaFile", config.mqttCaFile);
        config.mqttClientId = veda::detail::get_or<std::string>(j, "mqttClientId", config.mqttClientId);
        config.mqttKeepAliveSeconds = veda::detail::get_or<int>(j, "mqttKeepAliveSeconds", config.mqttKeepAliveSeconds);
        config.mqttReconnectDelaySeconds =
            veda::detail::get_or<int>(j, "mqttReconnectDelaySeconds", config.mqttReconnectDelaySeconds);
        config.mqttReconnectDelayMaxSeconds =
            veda::detail::get_or<int>(j, "mqttReconnectDelayMaxSeconds", config.mqttReconnectDelayMaxSeconds);
        config.mqttReceiverRetryIntervalMs =
            veda::detail::get_or<uint64_t>(j, "mqttReceiverRetryIntervalMs", config.mqttReceiverRetryIntervalMs);

        if (config.risk.dedupMergeDistance >= config.risk.warningDistance) {
            std::cerr << "[Config] 경고: dedupMergeDistance(" << config.risk.dedupMergeDistance
                      << ") >= warningDistance(" << config.risk.warningDistance
                      << ") — 정상 Danger 케이스가 dedup에 삼켜질 수 있습니다.\n";
        }

        for (int ch = 0; ch < config.channelCount; ++ch) {
            bool found = false;
            for (const auto& c : config.cameraCalibrations) {
                if (c.channelId == ch) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "[Config] 경고: 채널 " << ch << " 의 cameraCalibrations 항목 없음 — "
                          << "AffineLocalToWorldTransform 사용 시 이 채널은 좌표 변환 없이 통과됨\n";
            }
        }

        return config;
    }
};