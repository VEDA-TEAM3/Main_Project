#pragma once

/**
 * @file    AppConfig.h
 * @brief   연산 서버 구동에 필요한 전역 설정값
 */

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "Contract.h"

struct AppConfig {
    veda::ChannelId channelId = 0;

    /// @brief 전체 채널 수 (BlurFrame 유효성 검사 등 채널 범위 판단에 사용, [0, channelCount) 가정)
    int channelCount = 4;

    // Network
    std::string rtspUrl;
    int rtspLatencyMs = 200;
    std::string rtspIp;
    int rtspPort = 554;
    std::string rtspUser;
    std::string rtspPass;
    std::string rtspSetupUri;
    std::string rtspPlayUri;

    // Sanitize
    double sanitizerIouThresh = 0.5;
    double sanitizerContainThresh = 0.9;

    // Mapper
    double imageMapScaleX = 1.0;
    double imageMapScaleY = 1.0;
    double imageMapOffsetX = 0.0;
    double imageMapOffsetY = 0.0;

    // Homography
    std::array<double, 9> homography{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};

    // MQTT (공통 브로커 접속 정보) — MqttBlurSink/MqttTopViewSink가 각자의 mosquitto
    // 클라이언트로 동일한 브로커에 연결하므로, 접속 정보(호스트/포트/CA/keepalive/재시도
    // 간격)는 하나로 통일 (예전엔 Blur/TopView가 각각 중복 필드를 가져서 한쪽만 값을
    // 고치고 나머지를 빠뜨리면 서로 다른 브로커/인증서로 갈라지는 사고가 있었음)
    std::string mqttHost;
    int mqttPort = 8883;
    std::string mqttCaFile;
    int mqttKeepAliveSeconds = 30;

    /// @brief MqttBlurSink/MqttTopViewSink가 최초 연결에 실패했을 때 재시도하는 간격 (ms)
    int mqttRetryIntervalMs = 2000;

    // MQTT (Blur sink 전용) — clientId/큐 크기는 sink마다 독립적이어야 하므로 분리
    // (clientId는 두 sink가 같은 브로커에 서로 다른 연결로 붙으므로 반드시 달라야 함)
    std::string mqttBlurClientId;  ///< 비어있으면 MqttBlurSink가 자동 생성
    int mqttBlurMaxQueueSize = 8;

    // MQTT (TopView/risk sink 전용)
    std::string mqttTopViewClientId;  ///< 비어있으면 MqttTopViewSink가 자동 생성
    int mqttTopViewMaxQueueSize = 8;

    /**
     * @brief       외부 JSON 설정 파일에서 AppConfig를 로드하는 함수
     * @details     파일이 없거나 JSON 파싱 실패 시 기본값으로 계속 진행 (예외를 던지지 않음)
     * @param       configPath 설정 파일 경로
     * @return      파싱이 완료된 AppConfig 구조체
     */
    static inline AppConfig load(const std::string& configPath) {
        AppConfig cfg;

        std::ifstream file(configPath);
        if (!file.is_open()) {
            std::cerr << "[Config] 설정 파일을 열 수 없습니다: " << configPath << " — 기본값을 사용합니다.\n";
            return cfg;
        }

        nlohmann::json j;
        try {
            file >> j;
        } catch (const std::exception& e) {
            std::cerr << "[Config] JSON 파싱 실패: " << configPath << " (" << e.what() << ") — 기본값을 사용합니다.\n";
            return cfg;
        }

        cfg.channelId = veda::detail::get_or<veda::ChannelId>(j, "channelId", cfg.channelId);
        cfg.channelCount = veda::detail::get_or<int>(j, "channelCount", cfg.channelCount);

        cfg.rtspUrl = veda::detail::get_or<std::string>(j, "rtspUrl", cfg.rtspUrl);
        cfg.rtspLatencyMs = veda::detail::get_or<int>(j, "rtspLatencyMs", cfg.rtspLatencyMs);
        cfg.rtspIp = veda::detail::get_or<std::string>(j, "rtspIp", cfg.rtspIp);
        cfg.rtspPort = veda::detail::get_or<int>(j, "rtspPort", cfg.rtspPort);
        cfg.rtspUser = veda::detail::get_or<std::string>(j, "rtspUser", cfg.rtspUser);
        cfg.rtspPass = veda::detail::get_or<std::string>(j, "rtspPass", cfg.rtspPass);
        cfg.rtspSetupUri = veda::detail::get_or<std::string>(j, "rtspSetupUri", cfg.rtspSetupUri);
        cfg.rtspPlayUri = veda::detail::get_or<std::string>(j, "rtspPlayUri", cfg.rtspPlayUri);

        cfg.sanitizerIouThresh = veda::detail::get_or<double>(j, "sanitizerIouThresh", cfg.sanitizerIouThresh);
        cfg.sanitizerContainThresh =
            veda::detail::get_or<double>(j, "sanitizerContainThresh", cfg.sanitizerContainThresh);

        cfg.imageMapScaleX = veda::detail::get_or<double>(j, "imageMapScaleX", cfg.imageMapScaleX);
        cfg.imageMapScaleY = veda::detail::get_or<double>(j, "imageMapScaleY", cfg.imageMapScaleY);
        cfg.imageMapOffsetX = veda::detail::get_or<double>(j, "imageMapOffsetX", cfg.imageMapOffsetX);
        cfg.imageMapOffsetY = veda::detail::get_or<double>(j, "imageMapOffsetY", cfg.imageMapOffsetY);

        std::vector<double> homographyDefault(cfg.homography.begin(), cfg.homography.end());
        std::vector<double> homographyIn =
            veda::detail::get_or<std::vector<double>>(j, "homography", homographyDefault);
        if (homographyIn.size() == cfg.homography.size()) {
            std::copy(homographyIn.begin(), homographyIn.end(), cfg.homography.begin());
        } else if (homographyIn.size() != homographyDefault.size()) {
            std::cerr << "[Config] 경고: homography 배열 크기가 9가 아닙니다 (" << homographyIn.size()
                      << "개) — 기본값을 유지합니다.\n";
        }

        cfg.mqttHost = veda::detail::get_or<std::string>(j, "mqttHost", cfg.mqttHost);
        cfg.mqttPort = veda::detail::get_or<int>(j, "mqttPort", cfg.mqttPort);
        cfg.mqttCaFile = veda::detail::get_or<std::string>(j, "mqttCaFile", cfg.mqttCaFile);
        cfg.mqttKeepAliveSeconds = veda::detail::get_or<int>(j, "mqttKeepAliveSeconds", cfg.mqttKeepAliveSeconds);
        cfg.mqttRetryIntervalMs = veda::detail::get_or<int>(j, "mqttRetryIntervalMs", cfg.mqttRetryIntervalMs);

        cfg.mqttBlurClientId = veda::detail::get_or<std::string>(j, "mqttBlurClientId", cfg.mqttBlurClientId);
        cfg.mqttBlurMaxQueueSize = veda::detail::get_or<int>(j, "mqttBlurMaxQueueSize", cfg.mqttBlurMaxQueueSize);

        cfg.mqttTopViewClientId = veda::detail::get_or<std::string>(j, "mqttTopViewClientId", cfg.mqttTopViewClientId);
        cfg.mqttTopViewMaxQueueSize =
            veda::detail::get_or<int>(j, "mqttTopViewMaxQueueSize", cfg.mqttTopViewMaxQueueSize);

        return cfg;
    }
};