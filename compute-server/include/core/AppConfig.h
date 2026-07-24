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

    // Network (RTSP 접속 정보)
    // rtspUrl / rtspLatencyMs 는 읽기만 하고 아무도 쓰지 않는 죽은 필드였음 -> 삭제
    // (운영자가 rtspUrl 을 고치고 반영됐다고 착각하기 쉬운 함정. 실제 접속은
    //  rtspIp/rtspPort/rtspSetupUri/rtspPlayUri 조합으로만 이루어짐)
    std::string rtspIp;
    int rtspPort = 554;
    std::string rtspUser;
    std::string rtspPass;
    std::string rtspSetupUri;
    std::string rtspPlayUri;

    /**
     * @name RTSP 세션/소켓 튜닝
     * @details
     * 전부 예전엔 RtspClientV2 / RtspOnvifSourceV2 에 static constexpr 로 박혀 있던 값들
     * 기본값은 performance/compute-server.md 에 측정된 그 값 그대로이므로,
     * config 에서 건드리지 않으면 문서의 지표가 그대로 유효함
     *
     * @warning 버퍼 크기와 링 용량은 지연/메모리 트레이드오프에 직결됨
     *          바꾼 뒤에는 performance/compute-server.md 를 재측정할 것
     * @{
     */

    /// @brief connect() 최대 대기 시간 (초). SO_RCVTIMEO 는 connect 에 안 걸리므로 별도 필요
    int rtspConnectTimeoutSec = 5;

    /// @brief 수신 소켓 타임아웃 SO_RCVTIMEO (초). 스트림이 끊겼을 때 run() 이 빠져나오는 상한
    int rtspRecvTimeoutSec = 5;

    /// @brief 커널 수신 버퍼 SO_RCVBUF (bytes). burst 수신 시 커널 드롭 방지
    int rtspSocketRecvBufBytes = 1 << 20;

    /// @brief 사용자 공간 read 버퍼 (bytes). 이 크기 단위로 recv() 를 호출 -> syscall 수를 좌우
    int rtspReadBufBytes = 65536;

    /// @brief 재조합 중인 metadata frame 크기 상한 (bytes). 넘으면 marker 누락으로 보고 재연결 (OOM 방지)
    int rtspMaxMetadataFrameBytes = 1024 * 1024;

    /// @brief RTSP 세션 유지를 위한 GET_PARAMETER 전송 주기 (초)
    int rtspKeepAliveIntervalSec = 30;

    /// @brief 재연결 백오프 시작/상한 (초). 실패할 때마다 2배씩 증가
    int rtspReconnectBackoffInitialSec = 1;
    int rtspReconnectBackoffMaxSec = 30;

    /** @} */

    /**
     * @brief   Source 링버퍼 용량 (동시에 대기 가능한 최대 프레임 수)
     * @details 가득 차면 drop-oldest -> 지연이 무한정 누적되지 않음
     *          키우면 순간 burst 를 더 버티고, 줄이면 최신성이 올라감
     */
    int sourceRingCapacity = 8;

    /// @brief 네트워크/소스 성능 지표를 로그로 남기는 주기 (ms)
    int metricsReportIntervalMs = 5000;

    /**
     * @name 로깅
     * @details
     * 로그는 콘솔/파일 모두 백그라운드 워커가 배치로 내보내므로 호출 스레드에는 I/O가 없음
     * 다만 프레임마다 도는 이벤트(발행 성공, 팬텀 객체 제거, 라우팅 drop)는 Debug 레벨이라
     * 기본값(info)에서는 큐에도 들어가지 않음 -- 진단할 때만 debug 로 낮출 것
     * @{
     */

    /// @brief "debug" | "info" | "error" | "off"
    std::string logLevel = "info";

    /// @brief 콘솔 출력 여부 (journald 부하가 부담되면 끄고 CSV만 남길 수 있음)
    bool logToConsole = true;

    /// @brief "YYYY-MM-DD.csv" 파일 기록 여부
    bool logToFile = true;

    /// @brief 로그 워커가 큐를 비우는 주기 (ms)
    int logFlushIntervalMs = 500;

    /// @brief 로그 큐 상한 (초과 시 drop-oldest, 라즈베리파이 OOM 방지)
    int logMaxPendingEntries = 10000;

    /// @brief 로그 CSV 파일 이름 (작업 디렉터리 기준). 회전/압축/보존은 logrotate 가 담당하므로
    ///        반드시 고정 이름이어야 함 (날짜별 이름을 쓰면 logrotate 와 이중 회전이 됨)
    std::string logFileName = "veda.csv";

    /** @} */

    // Parser
    /**
     * @brief   bbox가 프레임 경계에 닿았다고 판정할 정규화 좌표 오차율
     * @details 예전엔 OnvifParser.cpp와 AffineImageCoordinateMapper.cpp가 각자
     *          kEdgeEpsilon 을 따로 정의하고 있어서 한쪽만 고치면 두 단계의 경계 판정이
     *          갈라졌음 -> 여기로 일원화하고 파서만 판정하도록 변경
     */
    double edgeEpsilon = 0.002;

    // Sanitize
    double sanitizerIouThresh = 0.5;
    double sanitizerContainThresh = 0.9;

    // Mapper (blur 경로 전용 — Metadata 좌표계 -> 앱이 그리는 RTSP 프레임 좌표계)
    double imageMapScaleX = 1.0;
    double imageMapScaleY = 1.0;
    double imageMapOffsetX = 0.0;
    double imageMapOffsetY = 0.0;

    /**
     * @name Homography (risk 경로 전용 — Metadata 이미지 평면 -> 카메라 로컬 지면 평면)
     * @{
     */

    /// @brief 행렬 9개 값 (row-major: h0..h2 / h3..h5 / h6..h8)
    std::array<double, 9> homography{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};

    /**
     * @brief   homography 행렬이 정의된 입력 좌표계
     * @details
     * - "normalized" : 입력이 [0,1] 정규화 좌표 (좌상단 원점)
     * - "pixel"      : 입력이 픽셀 좌표 (OpenCV findHomography() 의 기본 출력)
     *
     * @warning
     * 파이프라인이 변환기에 넘기는 지면점은 언제나 [0,1] 정규화 좌표임
     * "pixel" 로 지정하면 HomographyTransform 이 생성 시점에 딱 한 번
     * H_norm = H_pixel * diag(imageWidth, imageHeight, 1) 을 미리 곱해두므로
     * 런타임 비용은 0이고, 캘리브레이션 결과를 손으로 환산할 필요가 없음
     *
     * @note
     * 이 값을 틀리게 주면 예외도 경고도 없이 "그럴듯하지만 완전히 틀린" 좌표가 나옴
     * (h6/h7 분모 항의 스케일이 달라져 단순 배율 오차가 아니라 투영 자체가 깨짐)
     */
    std::string homographySpace = "normalized";

    /// @brief homographySpace == "pixel" 일 때 캘리브레이션에 쓴 이미지 해상도
    double imageWidth = 0.0;
    double imageHeight = 0.0;

    /** @} */

    /**
     * @name 카메라 로컬 좌표 유효 범위 (risk 경로 sanity check)
     * @details
     * 지평선 근처의 지면점은 분모가 0에 가까워 수백 미터 밖으로 발산함
     * 카메라가 커버하는 범위를 벗어난 좌표는 물리적으로 불가능하므로 버림
     * 도면 좌표가 아니라 카메라 로컬 기준임에 주의 (원점=카메라, +y=전방)
     * @{
     */
    bool localBoundsEnabled = false;
    double localMinX = 0.0;
    double localMaxX = 0.0;
    double localMinY = 0.0;
    double localMaxY = 0.0;
    /** @} */

    /**
     * @brief   bbox가 잘린 risk 객체를 어떻게 처리할지 결정하는 정책
     * @details
     * - "keep"                : 그대로 통과 (edge 플래그만 실어 보냄, 예전 동작)
     * - "dropBottomTruncated" : bbox 아래변이 잘린 객체만 버림 (기본값)
     * - "dropAnyEdge"         : 어느 변이든 경계에 닿으면 버림 (가장 보수적)
     *
     * @note [ 왜 아래변만 특별취급인가 ]
     * 지면점은 bbox 의 아래변 중앙임. 좌/우/위 변이 잘려도 발이 보이면 지면점은 여전히
     * 유효하지만, 아래변이 잘리면 발 위치를 모르는 채 잘린 지점을 지면으로 오인해
     * 실제보다 수 미터 먼 곳으로 사상됨 (IGroundPointExtractor.h 의 '잘림 현상')
     * 경계에 닿았다는 이유만으로 전부 버리면 화면에 막 진입한 사람을 통째로 놓치므로,
     * 좌표가 실제로 망가지는 경우만 골라내는 게 기본값
     */
    std::string riskEdgePolicy = "dropBottomTruncated";

    // MQTT (브로커 접속 정보) — 프로세스당 MqttTransport 인스턴스 하나가 소유하는
    // 단일 TLS 커넥션을 Blur/TopView 두 sink가 공유함
    // (예전엔 sink마다 mosquitto 클라이언트를 따로 만들어 채널당 커넥션 2개 + 네트워크
    //  스레드 2개를 썼고, 두 sink가 각자 mosquitto_lib_cleanup()을 불러 종료 시
    //  먼저 파괴된 쪽이 전역 상태를 해제해버리는 문제도 있었음)
    std::string mqttHost;
    int mqttPort = 8883;
    std::string mqttCaFile;
    int mqttKeepAliveSeconds = 30;

    /// @brief MqttTransport가 최초 연결에 실패했을 때 재시도하는 간격 (ms)
    int mqttRetryIntervalMs = 2000;

    /**
     * @brief   mosquitto 자동 재접속 대기 시간 (초, 지수 백오프)
     * @details 위의 mqttRetryIntervalMs 와 다름 -- 이쪽은 '한 번 붙었다가 끊긴' 경우를
     *          mosquitto 가 알아서 재시도하는 간격이고, mqttRetryIntervalMs 는
     *          '최초 접속조차 실패한' 경우 우리가 직접 도는 재시도 간격
     */
    int mqttReconnectDelaySec = 1;
    int mqttReconnectDelayMaxSec = 10;

    /**
     * @brief   MQTT clientId (비어있으면 MqttTransport가 자동 생성)
     * @details 커넥션이 프로세스당 하나로 합쳐졌으므로 clientId도 하나면 됨
     *          다만 채널마다 별도 프로세스이므로 채널 간에는 반드시 달라야 함
     *          (같은 clientId로 두 커넥션이 붙으면 브로커가 먼저 붙은 쪽을 끊음)
     */
    std::string mqttClientId;

    // 큐 크기는 sink마다 독립 — blur가 밀린다고 risk 발행까지 함께 지연되면 안 되므로
    // 전송 큐와 워커 스레드는 커넥션과 달리 공유하지 않음
    int mqttBlurMaxQueueSize = 8;
    int mqttTopViewMaxQueueSize = 8;

    /**
     * @brief   0 이하의 값이 들어오면 기본값으로 되돌리고 경고 (버퍼 크기/주기 등에 사용)
     * @details AppConfig::load 는 어떤 경우에도 예외를 던지지 않는다는 정책을 지키기 위한 헬퍼
     * @param   value    검사할 값 (필요 시 fallback 으로 덮어씀)
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
     *              값이 범위를 벗어나면 clampPositive 로 기본값을 되돌리고 경고만 남김
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

        if (j.contains("rtspUrl") || j.contains("rtspLatencyMs")) {
            std::cerr << "[Config] 경고: rtspUrl/rtspLatencyMs 는 실제로 사용되지 않아 삭제되었습니다 "
                         "— rtspIp/rtspPort/rtspSetupUri/rtspPlayUri 를 확인하세요.\n";
        }

        cfg.rtspIp = veda::detail::get_or<std::string>(j, "rtspIp", cfg.rtspIp);
        cfg.rtspPort = veda::detail::get_or<int>(j, "rtspPort", cfg.rtspPort);
        cfg.rtspUser = veda::detail::get_or<std::string>(j, "rtspUser", cfg.rtspUser);
        cfg.rtspPass = veda::detail::get_or<std::string>(j, "rtspPass", cfg.rtspPass);
        cfg.rtspSetupUri = veda::detail::get_or<std::string>(j, "rtspSetupUri", cfg.rtspSetupUri);
        cfg.rtspPlayUri = veda::detail::get_or<std::string>(j, "rtspPlayUri", cfg.rtspPlayUri);

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

        cfg.sourceRingCapacity = veda::detail::get_or<int>(j, "sourceRingCapacity", cfg.sourceRingCapacity);
        cfg.metricsReportIntervalMs =
            veda::detail::get_or<int>(j, "metricsReportIntervalMs", cfg.metricsReportIntervalMs);

        // 잘못된 값이 들어와도 절대 던지지 않고 안전한 범위로 되돌림
        clampPositive(cfg.rtspConnectTimeoutSec, 1, "rtspConnectTimeoutSec");
        clampPositive(cfg.rtspRecvTimeoutSec, 1, "rtspRecvTimeoutSec");
        clampPositive(cfg.rtspSocketRecvBufBytes, 1 << 20, "rtspSocketRecvBufBytes");
        clampPositive(cfg.rtspReadBufBytes, 65536, "rtspReadBufBytes");
        clampPositive(cfg.rtspMaxMetadataFrameBytes, 1024 * 1024, "rtspMaxMetadataFrameBytes");
        clampPositive(cfg.rtspKeepAliveIntervalSec, 30, "rtspKeepAliveIntervalSec");
        clampPositive(cfg.rtspReconnectBackoffInitialSec, 1, "rtspReconnectBackoffInitialSec");
        clampPositive(cfg.rtspReconnectBackoffMaxSec, 30, "rtspReconnectBackoffMaxSec");
        clampPositive(cfg.sourceRingCapacity, 8, "sourceRingCapacity");
        clampPositive(cfg.metricsReportIntervalMs, 5000, "metricsReportIntervalMs");

        if (cfg.rtspReconnectBackoffMaxSec < cfg.rtspReconnectBackoffInitialSec) {
            std::cerr << "[Config] 경고: rtspReconnectBackoffMaxSec 가 시작값보다 작습니다 — 시작값으로 맞춥니다.\n";
            cfg.rtspReconnectBackoffMaxSec = cfg.rtspReconnectBackoffInitialSec;
        }

        cfg.logLevel = veda::detail::get_or<std::string>(j, "logLevel", cfg.logLevel);
        if (cfg.logLevel != "debug" && cfg.logLevel != "info" && cfg.logLevel != "error" && cfg.logLevel != "off") {
            std::cerr << "[Config] 경고: logLevel=\"" << cfg.logLevel
                      << "\" 는 알 수 없는 값입니다 (debug|info|error|off) — \"info\"로 처리합니다.\n";
            cfg.logLevel = "info";
        }
        cfg.logToConsole = veda::detail::get_or<bool>(j, "logToConsole", cfg.logToConsole);
        cfg.logToFile = veda::detail::get_or<bool>(j, "logToFile", cfg.logToFile);
        cfg.logFlushIntervalMs = veda::detail::get_or<int>(j, "logFlushIntervalMs", cfg.logFlushIntervalMs);
        cfg.logMaxPendingEntries = veda::detail::get_or<int>(j, "logMaxPendingEntries", cfg.logMaxPendingEntries);
        cfg.logFileName = veda::detail::get_or<std::string>(j, "logFileName", cfg.logFileName);
        if (cfg.logFileName.empty()) {
            std::cerr << "[Config] 경고: logFileName 이 비어 있습니다 — 기본값(veda.csv)을 사용합니다.\n";
            cfg.logFileName = "veda.csv";
        }

        cfg.edgeEpsilon = veda::detail::get_or<double>(j, "edgeEpsilon", cfg.edgeEpsilon);

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
            std::cerr << "[Config] 경고: worldBounds 범위가 비어 있습니다 (max <= min) — 범위 검사를 끕니다.\n";
            cfg.localBoundsEnabled = false;
        }

        cfg.riskEdgePolicy = veda::detail::get_or<std::string>(j, "riskEdgePolicy", cfg.riskEdgePolicy);
        if (cfg.riskEdgePolicy != "keep" && cfg.riskEdgePolicy != "dropBottomTruncated" &&
            cfg.riskEdgePolicy != "dropAnyEdge") {
            std::cerr << "[Config] 경고: riskEdgePolicy=\"" << cfg.riskEdgePolicy
                      << "\" 는 알 수 없는 값입니다 — \"dropBottomTruncated\"로 처리합니다.\n";
            cfg.riskEdgePolicy = "dropBottomTruncated";
        }

        cfg.mqttHost = veda::detail::get_or<std::string>(j, "mqttHost", cfg.mqttHost);
        cfg.mqttPort = veda::detail::get_or<int>(j, "mqttPort", cfg.mqttPort);
        cfg.mqttCaFile = veda::detail::get_or<std::string>(j, "mqttCaFile", cfg.mqttCaFile);
        cfg.mqttKeepAliveSeconds = veda::detail::get_or<int>(j, "mqttKeepAliveSeconds", cfg.mqttKeepAliveSeconds);
        cfg.mqttRetryIntervalMs = veda::detail::get_or<int>(j, "mqttRetryIntervalMs", cfg.mqttRetryIntervalMs);

        cfg.mqttClientId = veda::detail::get_or<std::string>(j, "mqttClientId", cfg.mqttClientId);
        cfg.mqttReconnectDelaySec = veda::detail::get_or<int>(j, "mqttReconnectDelaySec", cfg.mqttReconnectDelaySec);
        cfg.mqttReconnectDelayMaxSec =
            veda::detail::get_or<int>(j, "mqttReconnectDelayMaxSec", cfg.mqttReconnectDelayMaxSec);
        clampPositive(cfg.mqttRetryIntervalMs, 2000, "mqttRetryIntervalMs");
        clampPositive(cfg.mqttReconnectDelaySec, 1, "mqttReconnectDelaySec");
        clampPositive(cfg.mqttReconnectDelayMaxSec, 10, "mqttReconnectDelayMaxSec");
        if (cfg.mqttReconnectDelayMaxSec < cfg.mqttReconnectDelaySec)
            cfg.mqttReconnectDelayMaxSec = cfg.mqttReconnectDelaySec;

        cfg.mqttBlurMaxQueueSize = veda::detail::get_or<int>(j, "mqttBlurMaxQueueSize", cfg.mqttBlurMaxQueueSize);
        cfg.mqttTopViewMaxQueueSize =
            veda::detail::get_or<int>(j, "mqttTopViewMaxQueueSize", cfg.mqttTopViewMaxQueueSize);

        if (j.contains("mqttBlurClientId") || j.contains("mqttTopViewClientId")) {
            std::cerr << "[Config] 경고: mqttBlurClientId/mqttTopViewClientId 는 더 이상 쓰이지 않습니다 "
                         "(커넥션이 하나로 합쳐짐) — mqttClientId 로 바꿔 주세요.\n";
        }

        return cfg;
    }
};