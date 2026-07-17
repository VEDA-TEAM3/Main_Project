#pragma once

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "Contract.h"

/**
 * @struct AppConfig
 * @brief 연산 서버 구동에 필요한 전역 설정값
 */
struct AppConfig {
    veda::ChannelId channelId = 0;

    std::string rtspUrl;      // 참고용 원본 URL
    int rtspLatencyMs = 200;  // GStreamer rtspsrc 전용 설정

    // RtspClient(Digest 인증 + TCP 인터리브)가 실제로 쓰는 연결 정보
    std::string rtspIp;
    int rtspPort = 554;
    std::string rtspUser;
    std::string rtspPass;
    std::string rtspSetupUri;  // 메타데이터 트랙 SETUP 요청 URI
    std::string rtspPlayUri;   // 세션 PLAY/GET_PARAMETER 요청에 쓰는 URI

    // ContainmentSanitizer 임계값
    double sanitizerIouThresh = 0.5;
    double sanitizerContainThresh = 0.9;

    // ONVIF 정규화 metadata -> 출력 RTSP 프레임 정규화 좌표.
    // output = input * scale + offset. 실측 검증 전에는 기본 항등값을 사용한다.
    double imageMapScaleX = 1.0;
    double imageMapScaleY = 1.0;
    double imageMapOffsetX = 0.0;
    double imageMapOffsetY = 0.0;

    // 출력 프레임 정규화 좌표 -> 월드 좌표(m) 호모그래피. row-major 3x3.
    bool homographyEnabled = false;
    std::array<double, 9> homography{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};

    // TODO: homography 3x3 행렬 (캘리브레이션 데이터 확보 후 추가)
    // TODO: sinks.risk / sinks.blur MQTT 브로커 주소 (통신 연동 시 추가)
};

namespace detail {

inline std::string trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
        return "";
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}
}  // namespace detail

/**
 * @brief   .env 파일에서 AppConfig를 로드하는 함수
 * @param   path .env 파일 경로
 * @return  파싱이 완료된 AppConfig 구조체
 */
inline AppConfig loadAppConfig(const std::string& path) {
    AppConfig cfg;
    std::ifstream file(path);

    if (!file.is_open()) {
        std::cerr << "[-] Failed to open config file: " << path << ". Using default values.\n";
        return cfg;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = detail::trim(line);

        if (line.empty() || line[0] == '#')
            continue;

        size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos)
            continue;

        std::string key = detail::trim(line.substr(0, delimiterPos));
        std::string value = detail::trim(line.substr(delimiterPos + 1));

        try {
            if (key == "CHANNEL_ID")
                cfg.channelId = std::stoi(value);
            else if (key == "RTSP_URL")
                cfg.rtspUrl = value;
            else if (key == "RTSP_IP")
                cfg.rtspIp = value;
            else if (key == "RTSP_PORT")
                cfg.rtspPort = std::stoi(value);
            else if (key == "RTSP_USER")
                cfg.rtspUser = value;
            else if (key == "RTSP_PASS")
                cfg.rtspPass = value;
            else if (key == "RTSP_SETUP_URI")
                cfg.rtspSetupUri = value;
            else if (key == "RTSP_PLAY_URI")
                cfg.rtspPlayUri = value;
            else if (key == "SANITIZER_IOU_THRESH")
                cfg.sanitizerIouThresh = std::stod(value);
            else if (key == "SANITIZER_CONTAIN_THRESH")
                cfg.sanitizerContainThresh = std::stod(value);
            else if (key == "IMAGE_MAP_SCALE_X")
                cfg.imageMapScaleX = std::stod(value);
            else if (key == "IMAGE_MAP_SCALE_Y")
                cfg.imageMapScaleY = std::stod(value);
            else if (key == "IMAGE_MAP_OFFSET_X")
                cfg.imageMapOffsetX = std::stod(value);
            else if (key == "IMAGE_MAP_OFFSET_Y")
                cfg.imageMapOffsetY = std::stod(value);
            else if (key == "HOMOGRAPHY_ENABLED")
                cfg.homographyEnabled = value == "1" || value == "true" || value == "TRUE";
            else if (key.rfind("HOMOGRAPHY_H", 0) == 0 && key.size() == 14 && key[12] >= '1' && key[12] <= '3' &&
                     key[13] >= '1' && key[13] <= '3')
                cfg.homography[(key[12] - '1') * 3 + (key[13] - '1')] = std::stod(value);
        } catch (const std::exception& e) {
            std::cerr << "[-] Failed to parse config value for key '" << key << "': " << e.what() << "\n";
        }
    }

    return cfg;
}