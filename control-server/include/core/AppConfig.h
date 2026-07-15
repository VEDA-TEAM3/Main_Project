#pragma once

/**
 * @file AppConfig.h
 * @brief 관제 서버의 전체 구동 설정값을 담는 구조체
 */

#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "Contract.h"

/**
 * @struct RiskConfig
 * @brief 위험도 판정 관련 거리 임계값
 * @note dedupMergeDistance 는 반드시 warningDistance 보다 작아야 함
 */
struct RiskConfig {
    double warningDistance = 5.0;     ///< m, 차량 기준 Warning 트리거 거리
    double dangerousDistance = 2.0;   ///< m, 차량 기준 Danger 트리거 거리
    double dedupMergeDistance = 1.0;  ///< m, 채널 간 dedup 병합 판정 거리
};

inline void from_json(const nlohmann::json& j, RiskConfig& r) {
    r.warningDistance = veda::detail::get_or<double>(j, "warningDistance", r.warningDistance);
    r.dangerousDistance = veda::detail::get_or<double>(j, "dangerousDistance", r.dangerousDistance);
    r.dedupMergeDistance = veda::detail::get_or<double>(j, "dedupMergeDistance", r.dedupMergeDistance);
}

/**
 * @struct ZoneBoundary
 * @brief 채널별 zone 배정 각도 구간 (atan2 기반, 원점 = 사거리 중심)
 * @note 전체 zoneBoundaries 의 각도 구간 합은 360도여야 함
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
 * @struct HwHealthCheckConfig
 * @brief STM32 하트비트 및 명령-상태 불일치 대응 정책
 */
struct HwHealthCheckConfig {
    uint32_t heartbeatIntervalMs = 500;        ///< STM32 → RPi 상태 보고 주기
    uint32_t missedBeatsForTimeout = 3;        ///< 연속 유실 시 채널 dead 판정 기준
    uint32_t mismatchRetryCount = 2;           ///< 명령-실제상태 불일치 시 재전송 횟수
    bool mismatchEscalateAfterRetries = true;  ///< 재시도 소진 시 대시보드 fault 표시 여부
};

inline void from_json(const nlohmann::json& j, HwHealthCheckConfig& h) {
    h.heartbeatIntervalMs = veda::detail::get_or<uint32_t>(j, "heartbeatIntervalMs", h.heartbeatIntervalMs);
    h.missedBeatsForTimeout = veda::detail::get_or<uint32_t>(j, "missedBeatsForTimeout", h.missedBeatsForTimeout);
    h.mismatchRetryCount = veda::detail::get_or<uint32_t>(j, "mismatchRetryCount", h.mismatchRetryCount);
    h.mismatchEscalateAfterRetries =
        veda::detail::get_or<bool>(j, "mismatchEscalateAfterRetries", h.mismatchEscalateAfterRetries);
}

struct AppConfig {
    // [파이프라인 설정]
    uint64_t windowSizeMs = 100;  ///< 프레임 집계 시간 윈도우 (ms)
    int channelCount = 4;         ///< 채널(zone) 개수. RiskEvaluation::zoneLevels 크기 결정에 사용

    // [위험도 정책 설정]
    RiskConfig risk;

    // [zone 배정 설정]
    std::vector<ZoneBoundary> zoneBoundaries;

    // [HW 헬스체크 설정]
    HwHealthCheckConfig hwHealthCheck;

    // [네트워크 설정]
    std::string mqttBrokerUrl = "tcp://localhost:1883";
    std::string mqttReceiveTopic = "veda/+/frame";
    std::string mqttSendTopic = "veda/server/merged";

    /**
     * @brief 외부 JSON 설정 파일에서 설정값을 읽어옴
     * @details 파일이 없거나 파싱 실패 시 기본값으로 계속 진행 (예외를 던지지 않음)
     * @param configPath 설정 파일 경로
     * @return AppConfig 파싱된 설정값 객체
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
        config.risk = veda::detail::get_or<RiskConfig>(j, "risk", config.risk);
        config.zoneBoundaries =
            veda::detail::get_or<std::vector<ZoneBoundary>>(j, "zoneBoundaries", config.zoneBoundaries);
        config.hwHealthCheck = veda::detail::get_or<HwHealthCheckConfig>(j, "hwHealthCheck", config.hwHealthCheck);
        config.mqttBrokerUrl = veda::detail::get_or<std::string>(j, "mqttBrokerUrl", config.mqttBrokerUrl);
        config.mqttReceiveTopic = veda::detail::get_or<std::string>(j, "mqttReceiveTopic", config.mqttReceiveTopic);
        config.mqttSendTopic = veda::detail::get_or<std::string>(j, "mqttSendTopic", config.mqttSendTopic);

        if (config.risk.dedupMergeDistance >= config.risk.warningDistance) {
            std::cerr << "[Config] 경고: dedupMergeDistance(" << config.risk.dedupMergeDistance
                      << ") >= warningDistance(" << config.risk.warningDistance
                      << ") — 정상 Danger 케이스가 dedup에 삼켜질 수 있습니다.\n";
        }

        return config;
    }
};