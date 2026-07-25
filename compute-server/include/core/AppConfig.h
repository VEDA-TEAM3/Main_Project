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
    // ==== Server Config ====
    // compute-server 는 엣지 워커라 자기 channelId 만 알면 된다. 전역 채널 총수(channelCount)는
    // 알 필요가 없다 -- 그걸 알면 CCTV 를 추가할 때마다 기존 모든 채널의 config 를 고쳐야 하므로
    // (전역 상태 = 스티칭/월드 좌표 매핑)는 control-server 의 책임이다.
    veda::ChannelId channelId = 0;

    // ==== RTSP Network Config ====
    std::string rtspIp;
    int rtspPort = 554;
    std::string rtspUser;
    std::string rtspPass;
    std::string rtspSetupUri;
    std::string rtspPlayUri;

    // ==== RTSP Policy Config ====
    int rtspConnectTimeoutSec = 5;                // connect() 최대 대기 시간 (초)
    int rtspRecvTimeoutSec = 5;                   // 수신 소켓 타임아웃 SO_RCVTIMEO (초)
    int rtspSocketRecvBufBytes = 1 << 20;         // 커널 수신 버퍼 SO_RCVBUF (bytes)
    int rtspReadBufBytes = 65536;                 // 사용자 공간 read 버퍼 (bytes)
    int rtspMaxMetadataFrameBytes = 1024 * 1024;  // 재조합 중인 metadata frame 크기 상한 (bytes)
    int rtspKeepAliveIntervalSec = 30;            // RTSP 세션 유지를 위한 GET_PARAMETER 전송 주기 (초)
    int rtspReconnectBackoffInitialSec = 1;       // 재연결 백오프 시작 (초)
    int rtspReconnectBackoffMaxSec = 30;          // 재연결 백오프 상한 (초)

    // ==== Network Config ====
    int sourceRingCapacity = 8;  // Source 링버퍼 용량

    // ==== Log Config ====
    int metricsReportIntervalMs = 5000;    // 네트워크/소스 성능 지표를 로그로 남기는 주기 (ms)
    std::string logLevel = "info";         // log 레벨 ( debug | info | error | off )
    bool logToConsole = true;              // 콘솔 출력 여부
    std::string logFileName = "veda.csv";  // 로그 CSV 파일 이름
    bool logToFile = true;                 // csv 파일 기록 여부
    int logFlushIntervalMs = 500;          // 로그 워커가 큐를 비우는 주기 (ms)
    int logMaxPendingEntries = 10000;      // 로그 큐 상한 (초과 시 drop-oldest, 라즈베리파이 OOM 방지)

    // ==== Parser Config ====
    double edgeEpsilon = 0.002;  // bbox가 프레임 경계에 닿았다고 판정할 정규화 좌표 오차율

    // ==== Sanitize Config ====
    double sanitizerIouThresh = 0.5;
    double sanitizerContainThresh = 0.9;

    // ==== Mapper Config ====
    double imageMapScaleX = 1.0;
    double imageMapScaleY = 1.0;
    double imageMapOffsetX = 0.0;
    double imageMapOffsetY = 0.0;

    // ==== Homography Config ====
    std::array<double, 9> homography{
        {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};  // Homography 행렬 (기본값: 항등 행렬)
    /*
        - "normalized" : 입력이 [0,1] 정규화 좌표
        - "pixel"      : 입력이 픽셀 좌표
     */
    std::string homographySpace = "normalized";  // homography 행렬이 정의된 입력 좌표계 ( normalized | pixel )
    // pixel 일 때 캘리브레이션에 쓴 이미지 해상도
    double imageWidth = 0.0;
    double imageHeight = 0.0;

    bool localBoundsEnabled = false;  // CCTV 로컬 좌표 유효 범위 설정
    // CCTV 로컬 좌표 유효 범위
    double localMinX = 0.0;
    double localMaxX = 0.0;
    double localMinY = 0.0;
    double localMaxY = 0.0;

    // ==== Router Config ====
    /*
        - "keep"                : 그대로 통과
        - "dropBottomTruncated" : bbox 아래변이 잘린 객체만 버림
        - "dropAnyEdge"         : 어느 변이든 경계에 닿으면 버림
     */
    std::string riskEdgePolicy = "dropBottomTruncated";  // bbox가 잘린 risk 객체를 어떻게 처리할지 결정하는 정책

    // ==== MQTT Broker Config ====
    std::string mqttHost;
    int mqttPort = 8883;
    std::string mqttCaFile;
    std::string mqttClientId;  // MQTT clientId (채널 간에는 반드시 달라야 함)

    // ==== MQTT Policy Config ====
    int mqttKeepAliveSeconds = 30;
    int mqttRetryIntervalMs = 2000;     // MqttTransport가 최초 연결에 실패했을 때 재시도하는 간격 (ms)
    int mqttReconnectDelaySec = 1;      // mosquitto 자동 재접속 시작 대기 시간 (초, 지수 백오프)
    int mqttReconnectDelayMaxSec = 10;  // mosquitto 자동 재접속 최대 대기 시간 (초, 지수 백오프)
    int mqttBlurMaxQueueSize = 8;       // blur Queue 크기
    int mqttTopViewMaxQueueSize = 8;    // Risk Queue 크기

    /**
     * @brief   0 이하의 값이 들어오면 기본값으로 되돌리고 경고 (버퍼 크기/주기 등에 사용)
     * @details AppConfig::load 는 어떤 경우에도 예외를 던지지 않는다는 정책을 지키기 위한 헬퍼
     * @param   value    검사할 값
     * @param   fallback 되돌릴 기본값
     * @param   name     경고 메시지에 쓸 설정 키 이름
     */
    static inline void clampPositive(int& value, int fallback, const char* name) {
        if (value <= 0) {
            std::cerr << "[Config] 경고: " << name << "=" << value << " 는 1 이상이어야 합니다 — 기본값(" << fallback
                      << ")을 사용합니다.\n";
            value = fallback;
        }
    }

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

        // Server Config
        cfg.channelId = veda::detail::get_or<veda::ChannelId>(j, "channelId", cfg.channelId);

        // RTSP Network Config
        cfg.rtspIp = veda::detail::get_or<std::string>(j, "rtspIp", cfg.rtspIp);
        cfg.rtspPort = veda::detail::get_or<int>(j, "rtspPort", cfg.rtspPort);
        cfg.rtspUser = veda::detail::get_or<std::string>(j, "rtspUser", cfg.rtspUser);
        cfg.rtspPass = veda::detail::get_or<std::string>(j, "rtspPass", cfg.rtspPass);
        cfg.rtspSetupUri = veda::detail::get_or<std::string>(j, "rtspSetupUri", cfg.rtspSetupUri);
        cfg.rtspPlayUri = veda::detail::get_or<std::string>(j, "rtspPlayUri", cfg.rtspPlayUri);

        // RTSP Policy Config
        cfg.rtspConnectTimeoutSec = veda::detail::get_or<int>(j, "rtspConnectTimeoutSec", cfg.rtspConnectTimeoutSec);
        cfg.rtspRecvTimeoutSec = veda::detail::get_or<int>(j, "rtspRecvTimeoutSec", cfg.rtspRecvTimeoutSec);
        cfg.rtspSocketRecvBufBytes = veda::detail::get_or<int>(j, "rtspSocketRecvBufBytes", cfg.rtspSocketRecvBufBytes);
        cfg.rtspReadBufBytes = veda::detail::get_or<int>(j, "rtspReadBufBytes", cfg.rtspReadBufBytes);
        cfg.rtspMaxMetadataFrameBytes =
            veda::detail::get_or<int>(j, "rtspMaxMetadataFrameBytes", cfg.rtspMaxMetadataFrameBytes);
        cfg.rtspKeepAliveIntervalSec =
            veda::detail::get_or<int>(j, "rtspKeepAliveIntervalSec", cfg.rtspKeepAliveIntervalSec);
        cfg.rtspReconnectBackoffInitialSec =
            veda::detail::get_or<int>(j, "rtspReconnectBackoffInitialSec", cfg.rtspReconnectBackoffInitialSec);
        cfg.rtspReconnectBackoffMaxSec =
            veda::detail::get_or<int>(j, "rtspReconnectBackoffMaxSec", cfg.rtspReconnectBackoffMaxSec);

        clampPositive(cfg.rtspConnectTimeoutSec, 1, "rtspConnectTimeoutSec");
        clampPositive(cfg.rtspRecvTimeoutSec, 1, "rtspRecvTimeoutSec");
        clampPositive(cfg.rtspSocketRecvBufBytes, 1 << 20, "rtspSocketRecvBufBytes");
        clampPositive(cfg.rtspReadBufBytes, 65536, "rtspReadBufBytes");
        clampPositive(cfg.rtspMaxMetadataFrameBytes, 1024 * 1024, "rtspMaxMetadataFrameBytes");
        clampPositive(cfg.rtspKeepAliveIntervalSec, 30, "rtspKeepAliveIntervalSec");
        clampPositive(cfg.rtspReconnectBackoffInitialSec, 1, "rtspReconnectBackoffInitialSec");
        clampPositive(cfg.rtspReconnectBackoffMaxSec, 30, "rtspReconnectBackoffMaxSec");

        if (cfg.rtspReconnectBackoffMaxSec < cfg.rtspReconnectBackoffInitialSec) {
            std::cerr << "[Config] 경고: rtspReconnectBackoffMaxSec 가 시작값보다 작습니다 — 시작값으로 맞춥니다.\n";
            cfg.rtspReconnectBackoffMaxSec = cfg.rtspReconnectBackoffInitialSec;
        }

        // Network Config
        cfg.sourceRingCapacity = veda::detail::get_or<int>(j, "sourceRingCapacity", cfg.sourceRingCapacity);
        clampPositive(cfg.sourceRingCapacity, 8, "sourceRingCapacity");

        // Log Config
        cfg.metricsReportIntervalMs =
            veda::detail::get_or<int>(j, "metricsReportIntervalMs", cfg.metricsReportIntervalMs);

        clampPositive(cfg.metricsReportIntervalMs, 5000, "metricsReportIntervalMs");
        cfg.logLevel = veda::detail::get_or<std::string>(j, "logLevel", cfg.logLevel);
        if (cfg.logLevel != "debug" && cfg.logLevel != "info" && cfg.logLevel != "error" && cfg.logLevel != "off") {
            std::cerr << "[Config] 경고: logLevel=\"" << cfg.logLevel
                      << "\" 는 알 수 없는 값입니다 (debug|info|error|off) — \"info\"로 처리합니다.\n";
            cfg.logLevel = "info";
        }
        cfg.logToConsole = veda::detail::get_or<bool>(j, "logToConsole", cfg.logToConsole);
        cfg.logFileName = veda::detail::get_or<std::string>(j, "logFileName", cfg.logFileName);
        if (cfg.logFileName.empty()) {
            std::cerr << "[Config] 경고: logFileName 이 비어 있습니다 — 기본값(veda.csv)을 사용합니다.\n";
            cfg.logFileName = "veda.csv";
        }
        cfg.logToFile = veda::detail::get_or<bool>(j, "logToFile", cfg.logToFile);
        cfg.logFlushIntervalMs = veda::detail::get_or<int>(j, "logFlushIntervalMs", cfg.logFlushIntervalMs);
        cfg.logMaxPendingEntries = veda::detail::get_or<int>(j, "logMaxPendingEntries", cfg.logMaxPendingEntries);

        // Parser Config
        cfg.edgeEpsilon = veda::detail::get_or<double>(j, "edgeEpsilon", cfg.edgeEpsilon);

        // Sanitize Config
        cfg.sanitizerIouThresh = veda::detail::get_or<double>(j, "sanitizerIouThresh", cfg.sanitizerIouThresh);
        cfg.sanitizerContainThresh =
            veda::detail::get_or<double>(j, "sanitizerContainThresh", cfg.sanitizerContainThresh);

        // Mapper Config
        cfg.imageMapScaleX = veda::detail::get_or<double>(j, "imageMapScaleX", cfg.imageMapScaleX);
        cfg.imageMapScaleY = veda::detail::get_or<double>(j, "imageMapScaleY", cfg.imageMapScaleY);
        cfg.imageMapOffsetX = veda::detail::get_or<double>(j, "imageMapOffsetX", cfg.imageMapOffsetX);
        cfg.imageMapOffsetY = veda::detail::get_or<double>(j, "imageMapOffsetY", cfg.imageMapOffsetY);

        // Homography Config
        std::vector<double> homographyDefault(cfg.homography.begin(), cfg.homography.end());
        std::vector<double> homographyIn =
            veda::detail::get_or<std::vector<double>>(j, "homography", homographyDefault);
        if (homographyIn.size() == cfg.homography.size()) {
            std::copy(homographyIn.begin(), homographyIn.end(), cfg.homography.begin());
        } else if (homographyIn.size() != homographyDefault.size()) {
            std::cerr << "[Config] 경고: homography 배열 크기가 9가 아닙니다 (" << homographyIn.size()
                      << "개) — 기본값을 유지합니다.\n";
        }
        cfg.homographySpace = veda::detail::get_or<std::string>(j, "homographySpace", cfg.homographySpace);
        if (cfg.homographySpace != "normalized" && cfg.homographySpace != "pixel") {
            std::cerr << "[Config] 경고: homographySpace=\"" << cfg.homographySpace
                      << "\" 는 알 수 없는 값입니다 (\"normalized\" | \"pixel\") — \"normalized\"로 처리합니다.\n";
            cfg.homographySpace = "normalized";
        }
        cfg.imageWidth = veda::detail::get_or<double>(j, "imageWidth", cfg.imageWidth);
        cfg.imageHeight = veda::detail::get_or<double>(j, "imageHeight", cfg.imageHeight);
        cfg.localBoundsEnabled = veda::detail::get_or<bool>(j, "localBoundsEnabled", cfg.localBoundsEnabled);
        cfg.localMinX = veda::detail::get_or<double>(j, "localMinX", cfg.localMinX);
        cfg.localMaxX = veda::detail::get_or<double>(j, "localMaxX", cfg.localMaxX);
        cfg.localMinY = veda::detail::get_or<double>(j, "localMinY", cfg.localMinY);
        cfg.localMaxY = veda::detail::get_or<double>(j, "localMaxY", cfg.localMaxY);
        if (cfg.localBoundsEnabled && (cfg.localMaxX <= cfg.localMinX || cfg.localMaxY <= cfg.localMinY)) {
            std::cerr << "[Config] 경고: localBounds 범위가 비어 있습니다 (max <= min) — 범위 검사를 끕니다.\n";
            cfg.localBoundsEnabled = false;
        }

        // Router Config
        cfg.riskEdgePolicy = veda::detail::get_or<std::string>(j, "riskEdgePolicy", cfg.riskEdgePolicy);
        if (cfg.riskEdgePolicy != "keep" && cfg.riskEdgePolicy != "dropBottomTruncated" &&
            cfg.riskEdgePolicy != "dropAnyEdge") {
            std::cerr << "[Config] 경고: riskEdgePolicy=\"" << cfg.riskEdgePolicy
                      << "\" 는 알 수 없는 값입니다 — \"dropBottomTruncated\"로 처리합니다.\n";
            cfg.riskEdgePolicy = "dropBottomTruncated";
        }

        // MQTT Broker Config
        cfg.mqttHost = veda::detail::get_or<std::string>(j, "mqttHost", cfg.mqttHost);
        cfg.mqttPort = veda::detail::get_or<int>(j, "mqttPort", cfg.mqttPort);
        cfg.mqttCaFile = veda::detail::get_or<std::string>(j, "mqttCaFile", cfg.mqttCaFile);
        cfg.mqttClientId = veda::detail::get_or<std::string>(j, "mqttClientId", cfg.mqttClientId);

        // MQTT Policy Config
        cfg.mqttKeepAliveSeconds = veda::detail::get_or<int>(j, "mqttKeepAliveSeconds", cfg.mqttKeepAliveSeconds);
        cfg.mqttRetryIntervalMs = veda::detail::get_or<int>(j, "mqttRetryIntervalMs", cfg.mqttRetryIntervalMs);
        cfg.mqttReconnectDelaySec = veda::detail::get_or<int>(j, "mqttReconnectDelaySec", cfg.mqttReconnectDelaySec);
        cfg.mqttReconnectDelayMaxSec =
            veda::detail::get_or<int>(j, "mqttReconnectDelayMaxSec", cfg.mqttReconnectDelayMaxSec);
        clampPositive(cfg.mqttRetryIntervalMs, 2000, "mqttRetryIntervalMs");
        clampPositive(cfg.mqttReconnectDelaySec, 1, "mqttReconnectDelaySec");
        clampPositive(cfg.mqttReconnectDelayMaxSec, 10, "mqttReconnectDelayMaxSec");
        if (cfg.mqttReconnectDelayMaxSec < cfg.mqttReconnectDelaySec) {
            cfg.mqttReconnectDelayMaxSec = cfg.mqttReconnectDelaySec;
        }
        cfg.mqttBlurMaxQueueSize = veda::detail::get_or<int>(j, "mqttBlurMaxQueueSize", cfg.mqttBlurMaxQueueSize);
        cfg.mqttTopViewMaxQueueSize =
            veda::detail::get_or<int>(j, "mqttTopViewMaxQueueSize", cfg.mqttTopViewMaxQueueSize);

        return cfg;
    }
};