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

    // MQTT
    std::string mqttHost;
    int mqttPort = 0;
    std::string mqttCaFile = "/etc/veda/certs/ca.crt";
    std::string mqttClientId;
    int mqttKeepAliveSeconds = 30;
    std::size_t mqttMaxQueueSize = 8;

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

        cfg.rtspUrl = veda::detail::get_or<std::string>(j, "rtspUrl", cfg.rtspUrl);
        cfg.rtspLatencyMs = veda::detail::get_or<int>(j, "rtspLatencyMs", cfg.rtspLatencyMs);
        cfg.rtspIp = veda::detail::get_or<std::string>(j, "rtspIp", cfg.rtspIp);
        cfg.rtspPort = veda::detail::get_or<int>(j, "rtspPort", cfg.rtspPort);
        cfg.rtspUser = veda::detail::get_or<std::string>(j, "rtspUser", cfg.rtspUser);
        cfg.rtspPass = veda::detail::get_or<std::string>(j, "rtspPass", cfg.rtspPass);
        cfg.rtspSetupUri = veda::detail::get_or<std::string>(j, "rtspSetupUri", cfg.rtspSetupUri);
        cfg.rtspPlayUri = veda::detail::get_or<std::string>(j, "rtspPlayUri", cfg.rtspPlayUri);

        cfg.mqttHost = veda::detail::get_or<std::string>(j, "mqttHost", cfg.mqttHost);
        cfg.mqttPort = veda::detail::get_or<int>(j, "mqttPort", cfg.mqttPort);
        cfg.mqttCaFile = veda::detail::get_or<std::string>(j, "mqttCaFile", cfg.mqttCaFile);
        cfg.mqttClientId = veda::detail::get_or<std::string>(j, "mqttClientId", cfg.mqttClientId);
        cfg.mqttKeepAliveSeconds =
            veda::detail::get_or<int>(j, "mqttKeepAliveSeconds", cfg.mqttKeepAliveSeconds);
        cfg.mqttMaxQueueSize =
            veda::detail::get_or<std::size_t>(j, "mqttMaxQueueSize", cfg.mqttMaxQueueSize);

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

        return cfg;
    }
};
