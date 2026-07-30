#pragma once

/**
 * @file    AppConfig.h
 * @brief   연산 서버 구동에 필요한 전역 설정값
 */

#include <algorithm>
#include <array>
#include <cmath>
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

    /**
     * @brief   blur 박스 확대 배율 (지연 보상 여유, 1.0 = 확대 안 함)
     *
     * @details
     * 전송/렌더 지연 동안 빠른 객체가 박스를 벗어나는 것을 트래킹 없이 보정하기 위한 여유폭.
     * 중심을 고정한 채 폭/높이에만 곱하므로 1.2 는 '면적 1.44 배'다 -- 과하게 키우면
     * 가려야 할 대상 주변까지 뭉개진다
     * 하한이 1.0 인 이유: 1.0 미만은 박스를 '줄여서' 얼굴/번호판을 노출시키는 방향이며,
     * 이는 설정 실수로도 절대 도달하면 안 되는 값이다 (blur 는 프라이버시 기능)
     */
    double blurBoxScale = 1.25;

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
    int mqttRetryIntervalMs = 2000;  // MqttTransport가 최초 연결에 실패했을 때 재시도하는 간격 (ms)
    int mqttReconnectDelaySec = 1;   // mosquitto 자동 재접속 시작 대기 시간 (초, 지수 백오프)
    int mqttReconnectDelayMaxSec = 10;  // mosquitto 자동 재접속 최대 대기 시간 (초, 지수 백오프)
    int mqttBlurMaxQueueSize = 8;       // blur Queue 크기 (1..kMaxMqttQueueSize 로 clamp)
    int mqttTopViewMaxQueueSize = 8;    // Risk Queue 크기 (1..kMaxMqttQueueSize 로 clamp)

    /**
     * @brief   Sink 전송 큐 길이의 절대 상한
     *
     * @details
     * 큐 하나가 붙잡을 수 있는 최악 상주 메모리는 (길이 × 프레임당 객체 상한 × 객체 크기) 다.
     * 1000 × 256 × 약 40B ≈ 10MB/Sink -- 라즈베리파이에서 두 Sink 를 합쳐도 감당 가능한 선.
     * 상한이 없으면 '프레임을 덜 버리려고' 큐를 키우는 자연스러운 대응이 그대로 OOM 이 된다
     */
    static constexpr int kMaxMqttQueueSize = 1000;

    /**
     * @brief   blur 박스 확대 배율의 절대 상한
     * @details 2.0 이면 면적 4 배 -- 그 이상은 '지연 보정'이 아니라 화면을 통째로 가리는 것에 가깝다
     */
    static constexpr double kMaxBlurBoxScale = 2.0;

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
     * @brief   값을 [minValue, maxValue] 로 잘라내고 경고 (상한이 있어야 하는 큐/버퍼 길이에 사용)
     *
     * @details
     * clampPositive 는 하한만 본다. 그러나 '메모리를 직접 차지하는 길이' 설정은 상한도 필요하다
     * -- Sink 큐는 (길이 × 프레임당 객체 상한 256 × 객체 크기) 만큼 상주할 수 있으므로,
     *    상한이 없으면 config.json 한 줄로 프로세스가 OOM 에 도달한다
     * 기본값으로 되돌리지 않고 '경계값으로 자르는' 이유: 운영자가 큐를 키우려던 의도 자체는
     * 살려주는 편이 낫고, 기본값(8)으로 되돌리면 의도와 정반대로 더 많이 버리게 되기 때문
     *
     * @param   value    검사할 값
     * @param   minValue 허용 하한
     * @param   maxValue 허용 상한
     * @param   name     경고 메시지에 쓸 설정 키 이름
     */
    static inline void clampRange(int& value, int minValue, int maxValue, const char* name) {
        if (value < minValue) {
            std::cerr << "[Config] 경고: " << name << "=" << value << " 는 " << minValue << " 이상이어야 합니다 — "
                      << minValue << " 로 조정합니다.\n";
            value = minValue;
        } else if (value > maxValue) {
            std::cerr << "[Config] 경고: " << name << "=" << value << " 는 " << maxValue << " 이하여야 합니다 — "
                      << maxValue << " 로 조정합니다.\n";
            value = maxValue;
        }
    }

    /**
     * @brief   clampRange 의 double 오버로드 (배율/비율처럼 정수가 아닌 상·하한 설정에 사용)
     * @details 비유한값(NaN/Inf)은 어떤 비교에도 false 이므로 별도로 걸러 기본값 의미의 minValue 로 되돌린다
     */
    static inline void clampRange(double& value, double minValue, double maxValue, const char* name) {
        if (!std::isfinite(value)) {
            std::cerr << "[Config] 경고: " << name << " 가 유한한 수가 아닙니다 — " << minValue << " 로 조정합니다.\n";
            value = minValue;
        } else if (value < minValue) {
            std::cerr << "[Config] 경고: " << name << "=" << value << " 는 " << minValue << " 이상이어야 합니다 — "
                      << minValue << " 로 조정합니다.\n";
            value = minValue;
        } else if (value > maxValue) {
            std::cerr << "[Config] 경고: " << name << "=" << value << " 는 " << maxValue << " 이하여야 합니다 — "
                      << maxValue << " 로 조정합니다.\n";
            value = maxValue;
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
        cfg.blurBoxScale = veda::detail::get_or<double>(j, "blurBoxScale", cfg.blurBoxScale);
        clampRange(cfg.blurBoxScale, 1.0, kMaxBlurBoxScale, "blurBoxScale");

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

        // [S1] 큐 길이는 상주 메모리를 직접 결정하므로 하한뿐 아니라 상한도 강제한다.
        // 상한 kMaxMqttQueueSize=1000 기준 최악 상주 = 1000 × 256(프레임당 객체 상한) × 약 40B ≈ 10MB/Sink.
        // 예전에는 이 두 개만 clamp 를 거치지 않아, 하한은 Sink 가 std::max(1, ...) 로 '경고 없이' 보정하고
        // 상한은 아예 없어서 mqttTopViewMaxQueueSize=100000 한 줄로 OOM 에 도달할 수 있었다
        clampRange(cfg.mqttBlurMaxQueueSize, 1, kMaxMqttQueueSize, "mqttBlurMaxQueueSize");
        clampRange(cfg.mqttTopViewMaxQueueSize, 1, kMaxMqttQueueSize, "mqttTopViewMaxQueueSize");

        return cfg;
    }
};