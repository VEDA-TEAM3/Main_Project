#pragma once

/**
 * @file    AppConfig.h
 * @brief   stream-server 구동 설정 + 신뢰 경계 입력 검증
 *
 * @note 다른 두 서버와 같은 계약: load() 는 절대 예외를 던지지 않는다.
 *       조립 시점 오류(잘못된 URI 등)는 생성자가 던지고 main 이 잡아 종료한다.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "Contract.h"

/// @brief RTSP URI 최대 길이. 경로/쿼리를 넉넉히 잡아도 이 이상은 정상 카메라가 아니다
inline constexpr std::size_t kMaxUriLength = 1024;
inline constexpr std::size_t kMaxPathLength = 512;

/**
 * @name 신뢰 경계 검증 (config.json 은 운영자가 쓰지만 신뢰 입력이 아니다)
 *
 * @warning [ 왜 이게 '있으면 좋은 것'이 아니라 필수인가 ]
 * `gst_parse_launch` 는 **문자열을 파싱해 엘리먼트 그래프를 만든다.** URI 를 그 문자열에
 * 끼워 넣으면 `!` 나 공백을 담은 값 하나로 임의 엘리먼트를 주입할 수 있다
 * (예: `rtsp://x ! filesink location=/etc/passwd`). 실제 파일 쓰기/네트워크 전송까지
 * 이어지는 실행 경로다.
 *
 * 이 서버는 **URI 를 launch 문자열에 넣지 않는 방식**으로 그 경로를 구조적으로 없앤다
 * (RtspSource.cpp 참고 — `g_object_set(src, "location", ...)`). 아래 검증은 그 위에
 * 얹는 2차 방어이며, 오타를 기동 시점에 잡아 주는 역할도 겸한다.
 * @{
 */

/// @brief 제어문자·공백·따옴표·파이프 등 파이프라인 문법에 의미를 갖는 문자를 거부
inline bool hasUnsafeCharacters(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        if (c < 0x20 || c == 0x7F) {
            return true;  // 제어문자 (개행으로 launch 문자열을 쪼개는 시도 포함)
        }
        switch (c) {
            case ' ':
            case '\t':
            case '!':
            case '"':
            case '\'':
            case '\\':
            case '(':
            case ')':
            case ';':
            case ',':
                return true;
            default:
                return false;
        }
    });
}

/// @brief rtsp:// 또는 rtsps:// 스킴에 길이/문자 제약을 만족하는지
inline bool isSafeRtspUri(const std::string& uri) {
    if (uri.empty() || uri.size() > kMaxUriLength) {
        return false;
    }
    const bool schemeOk = uri.rfind("rtsp://", 0) == 0 || uri.rfind("rtsps://", 0) == 0;
    if (!schemeOk) {
        return false;
    }
    return !hasUnsafeCharacters(uri);
}

/// @brief RTSP 마운트 경로 ("/ch0"). 절대경로 1단계만 허용하고 상위 탐색을 막는다
inline bool isSafeMountPoint(const std::string& mount) {
    if (mount.size() < 2 || mount.size() > 64 || mount.front() != '/') {
        return false;
    }
    if (mount.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(mount.begin() + 1, mount.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

/// @brief 세그먼트 디렉터리. 절대경로 + 상위 탐색 금지 (경로 조작으로 시스템 파일 덮어쓰기 방지)
inline bool isSafeDirectory(const std::string& path) {
    if (path.empty() || path.size() > kMaxPathLength || path.front() != '/') {
        return false;
    }
    if (path.find("..") != std::string::npos) {
        return false;
    }
    return !hasUnsafeCharacters(path);
}

/** @} */

/**
 * @brief 채널 1개의 수집/녹화/중계 설정
 */
struct ChannelConfig {
    veda::ChannelId channelId = -1;

    std::string rtspUri;   ///< 카메라 RTSP URI. launch 문자열에 **넣지 않는다** (속성으로 주입)
    std::string username;  ///< 비면 인증 없음
    std::string password;

    /// @brief "h264" | "h265" — depay/parse/pay 엘리먼트 선택에 쓰인다
    std::string codec = "h264";

    std::string mountPoint = "/ch0";  ///< Qt 가 붙을 경로: rtsp://<rpi>:<port><mountPoint>

    std::string segmentDir;             ///< 세그먼트 저장 디렉터리 (절대경로)
    std::uint32_t segmentSeconds = 60;  ///< 세그먼트 1개 길이

    /// @name 보존 상한 — 둘 중 **먼저 걸리는 쪽**으로 오래된 것부터 지운다
    /// @details 디스크가 차면 splitmuxsink 가 죽고 tee 로 묶인 중계까지 함께 멈춘다
    /// @{
    std::uint64_t retentionBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;  ///< 채널당 8 GiB
    std::uint32_t retentionSegments = 720;                              ///< 60초 × 720 = 12시간
    /// @}

    bool valid() const {
        return channelId >= 0 && isSafeRtspUri(rtspUri) && isSafeMountPoint(mountPoint) &&
               isSafeDirectory(segmentDir) && (codec == "h264" || codec == "h265") && segmentSeconds > 0;
    }
};

inline void from_json(const nlohmann::json& j, ChannelConfig& c) {
    c.channelId = veda::detail::get_or<veda::ChannelId>(j, "channelId", -1);
    c.rtspUri = veda::detail::get_or<std::string>(j, "rtspUri", "");
    c.username = veda::detail::get_or<std::string>(j, "username", "");
    c.password = veda::detail::get_or<std::string>(j, "password", "");
    c.codec = veda::detail::get_or<std::string>(j, "codec", c.codec);
    c.mountPoint = veda::detail::get_or<std::string>(j, "mountPoint", c.mountPoint);
    c.segmentDir = veda::detail::get_or<std::string>(j, "segmentDir", "");
    c.segmentSeconds = veda::detail::get_or<std::uint32_t>(j, "segmentSeconds", c.segmentSeconds);
    c.retentionBytes = veda::detail::get_or<std::uint64_t>(j, "retentionBytes", c.retentionBytes);
    c.retentionSegments = veda::detail::get_or<std::uint32_t>(j, "retentionSegments", c.retentionSegments);
    std::transform(c.codec.begin(), c.codec.end(), c.codec.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
}

struct AppConfig {
    std::vector<ChannelConfig> channels;

    int rtspServerPort = 8554;

    /// @name RTSP 수집 튜닝 (초저지연)
    /// @{
    std::uint32_t rtspLatencyMs = 100;  ///< rtspsrc 지터버퍼. 낮을수록 지연↓ 손실 복원↓
    bool rtspOverTcp = true;            ///< UDP 손실은 녹화 파일을 조용히 깨뜨린다
    /// @}

    /// @name 재연결 백오프
    /// @{
    std::uint32_t reconnectInitialSec = 1;
    std::uint32_t reconnectMaxSec = 30;
    /// @}

    std::string logLevel = "info";
    bool logToConsole = true;
    bool logToFile = true;
    std::string logFileName = "veda-stream.csv";
    int logFlushIntervalMs = 500;
    int logMaxPendingEntries = 10000;

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
            std::cerr << "[Config] JSON 파싱 실패: " << configPath << " (" << e.what()
                      << ") — 기본값을 사용합니다.\n";
            return config;
        }

        config.channels = veda::detail::get_or<std::vector<ChannelConfig>>(j, "channels", config.channels);
        config.rtspServerPort = veda::detail::get_or<int>(j, "rtspServerPort", config.rtspServerPort);
        if (config.rtspServerPort < 1024 || config.rtspServerPort > 65535) {
            std::cerr << "[Config] 경고: rtspServerPort=" << config.rtspServerPort
                      << " 가 [1024, 65535] 밖입니다 — 8554 로 보정합니다.\n";
            config.rtspServerPort = 8554;
        }

        config.rtspLatencyMs = veda::detail::get_or<std::uint32_t>(j, "rtspLatencyMs", config.rtspLatencyMs);
        config.rtspLatencyMs = std::min<std::uint32_t>(config.rtspLatencyMs, 2000);
        config.rtspOverTcp = veda::detail::get_or<bool>(j, "rtspOverTcp", config.rtspOverTcp);

        config.reconnectInitialSec =
            std::max<std::uint32_t>(1, veda::detail::get_or<std::uint32_t>(j, "reconnectInitialSec", 1));
        config.reconnectMaxSec =
            std::max(config.reconnectInitialSec, veda::detail::get_or<std::uint32_t>(j, "reconnectMaxSec", 30));

        config.logLevel = veda::detail::get_or<std::string>(j, "logLevel", config.logLevel);
        if (config.logLevel != "debug" && config.logLevel != "info" && config.logLevel != "error" &&
            config.logLevel != "off") {
            std::cerr << "[Config] 경고: logLevel=\"" << config.logLevel << "\" 알 수 없음 — \"info\" 로 처리합니다.\n";
            config.logLevel = "info";
        }
        config.logToConsole = veda::detail::get_or<bool>(j, "logToConsole", config.logToConsole);
        config.logToFile = veda::detail::get_or<bool>(j, "logToFile", config.logToFile);
        config.logFileName = veda::detail::get_or<std::string>(j, "logFileName", config.logFileName);
        config.logFlushIntervalMs = veda::detail::get_or<int>(j, "logFlushIntervalMs", config.logFlushIntervalMs);
        config.logMaxPendingEntries =
            veda::detail::get_or<int>(j, "logMaxPendingEntries", config.logMaxPendingEntries);

        // [검증] 잘못된 채널은 잘라내지 않고 '제거'한다. 잘라서 반쯤 살려두면 어떤 채널이
        // 왜 안 나오는지 현장에서 추적이 안 된다.
        std::vector<ChannelConfig> valid;
        valid.reserve(config.channels.size());
        for (std::size_t i = 0; i < config.channels.size(); ++i) {
            const ChannelConfig& channel = config.channels[i];
            if (!channel.valid()) {
                std::cerr << "[Config] 경고: channels[" << i << "] (channelId=" << channel.channelId
                          << ") 검증 실패 — 이 채널을 제거합니다."
                          << " (rtspUri 스킴/문자, mountPoint, segmentDir 절대경로, codec 확인)\n";
                continue;
            }
            const bool duplicate = std::any_of(valid.begin(), valid.end(), [&](const ChannelConfig& seen) {
                return seen.channelId == channel.channelId || seen.mountPoint == channel.mountPoint;
            });
            if (duplicate) {
                std::cerr << "[Config] 경고: channels[" << i << "] 의 channelId/mountPoint 가 중복 — 제거합니다.\n";
                continue;
            }
            valid.push_back(channel);
        }
        config.channels.swap(valid);

        if (config.channels.empty()) {
            std::cerr << "[Config] 경고: 유효한 채널이 하나도 없습니다 — 수집 없이 기동합니다.\n";
        }
        return config;
    }
};
