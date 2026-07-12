/**
 * @file    AppConfig.h
 * @brief   관제 서버에 필요한 값을 설정하는 파일
 */
#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

/**
 * @brief   위험 거리 임계값 설정
 * @note    단위는 Top-view 좌표계 기준 미터(m)이며,
 *          반드시 dangerDistance < warningDistance 를 만족해야 한다.
 *          위반 시 AppConfig::fromEnv() 단계에서 에러 출력 후 프로세스를 종료한다.
 */
struct RiskThresholdConfig {
    double warningDistance; /**< WARNING 판정 거리 제한 (m). 이 값 미만이면 WARNING */
    double dangerDistance;  /**< DANGER 판정 거리 제한 (m). 이 값 미만이면 DANGER */
};

/**
 * @brief   관제 서버에 필요한 설정을 담은 구조체
 * @note    각 구현체(INetwork, IAppSender, IDriverSender 등) 고유 설정(브로커 주소,
 *          토픽, dlopen 경로 등)은 여기 포함하지 않는다. 해당 구현체가 자체적으로 소유한다.
 */
struct AppConfig {
    RiskThresholdConfig riskThreshold; /**< 위험 거리 임계값 */
    uint32_t frameIntervalMs = 100;    /**< FrameAggregator 스냅샷 생성 주기 (ms) */

    /**
     * @brief   환경 변수로부터 값을 읽어 AppConfig를 생성한다.
     * @note    필요한 환경 변수: RISK_WARNING_DISTANCE, RISK_DANGER_DISTANCE,
     *          FRAME_INTERVAL_MS(선택, 미지정 시 기본값 100 사용).
     *          RISK_WARNING_DISTANCE, RISK_DANGER_DISTANCE 중 하나라도 비어있거나,
     *          숫자로 변환할 수 없거나, dangerDistance >= warningDistance이면
     *          에러 메시지를 출력하고 std::exit(EXIT_FAILURE)로 종료한다.
     * @return  환경 변수 값으로 채워진 AppConfig 인스턴스
     */
    static AppConfig fromEnv() {
        auto get_env_safe = [](const char* key) -> std::string {
            const char* val = std::getenv(key);
            return (val == nullptr) ? "" : std::string(val);
        };

        std::string warningStr = get_env_safe("RISK_WARNING_DISTANCE");
        std::string dangerStr = get_env_safe("RISK_DANGER_DISTANCE");
        std::string frameIntervalStr = get_env_safe("FRAME_INTERVAL_MS");

        if (warningStr.empty() || dangerStr.empty()) {
            std::cerr << "[-] Fatal Error: Missing required environment variables "
                         "(RISK_WARNING_DISTANCE, RISK_DANGER_DISTANCE).\n";
            std::exit(EXIT_FAILURE);
        }

        AppConfig cfg;
        try {
            cfg.riskThreshold.warningDistance = std::stod(warningStr);
            cfg.riskThreshold.dangerDistance = std::stod(dangerStr);
            if (!frameIntervalStr.empty()) {
                cfg.frameIntervalMs = static_cast<uint32_t>(std::stoul(frameIntervalStr));
            }
        } catch (const std::exception& e) {
            std::cerr << "[-] Fatal Error: Invalid environment variable value: " << e.what() << "\n";
            std::exit(EXIT_FAILURE);
        }

        if (cfg.riskThreshold.dangerDistance >= cfg.riskThreshold.warningDistance) {
            std::cerr << "[-] Fatal Error: RISK_DANGER_DISTANCE must be less than "
                         "RISK_WARNING_DISTANCE.\n";
            std::exit(EXIT_FAILURE);
        }

        return cfg;
    }
};