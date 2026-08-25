#pragma once

/**
 * @file    AppConfig.h
 * @brief   관제 서버의 전체 구동 설정값을 담는 구조체
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
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
     * @brief   같은 실체를 프레임 간에 이어붙일 최대 이동 거리 (m)
     *
     * @details
     * 이 거리 안에서 같은 클래스면 이전 프레임의 GlobalId 를 물려받는다.
     * 0 이하면 추적을 끄고 매 프레임 새 gid 를 부여한다 (예전 동작).
     *
     * @warning [ windowSizeMs / 소스 프레임률과의 묵시적 결합 ]
     * 이 값은 단독으로 정할 수 없다. 반드시 다음을 만족해야 한다:
     *
     *     trackMaxDistance >= v_max * max(windowSizeMs, T_src)
     *
     * 추적 가능한 최대 속도가 곧 (이 값 / 프레임 간격) 이기 때문이다. 위반하면 그 속도를
     * 넘는 객체는 **매 프레임 새 gid 를 받는다** -- 컴파일 오류도 런타임 오류도 로그도 없이
     * UI 에서 객체가 사라졌다 다시 생기는 것처럼 보인다.
     *
     * 현재 값 4.0m 산정 근거: windowSizeMs=220ms 기준 4.0/0.22 = 18.2m/s = 약 65km/h.
     * windowSizeMs 를 올리면 이 값도 같이 올려야 한다.
     * 자세한 내용은 CLAUDE.md 의 동명 불변식 항목 참고.
     */
    double trackMaxDistance = 4.0;

    /**
     * @brief   같은 gid의 정지 좌표 잡음을 고정하는 공간 히스테리시스 반경(m)
     * @details 이전 출력에서 이 반경 안의 변화는 고정하고, 반경을 넘는 실제 이동은 원시 좌표와의
     *          최대 공간 지연이 이 값 이하가 되도록 따라감. 0 이면 비활성화
     */
    double positionJitterRadius = 0.15;
};

inline void from_json(const nlohmann::json& j, RiskConfig& r) {
    r.warningDistance = veda::detail::get_or<double>(j, "warningDistance", r.warningDistance);
    r.dangerousDistance = veda::detail::get_or<double>(j, "dangerousDistance", r.dangerousDistance);
    r.dedupMergeDistance = veda::detail::get_or<double>(j, "dedupMergeDistance", r.dedupMergeDistance);
    r.trackMaxDistance = veda::detail::get_or<double>(j, "trackMaxDistance", r.trackMaxDistance);
    r.positionJitterRadius = veda::detail::get_or<double>(j, "positionJitterRadius", r.positionJitterRadius);
}

/**
 * @brief   월드 좌표를 하드웨어 zone 에 배정하는 zone 설정
 *
 * @details 방향 모드는 좌표 범위 합집합을 CCTV 커버리지로 사용한다. 커버리지 안에서는 가장
 *          가까운 물리 CCTV를 먼저 선택하고 해당 CCTV의 4개 방향 채널 중 투영 점수가 가장
 *          높은 채널을 배정한다.
 *
 * @note    zoneId 는 하드웨어 액추에이터 채널과 동일 정수다 (zoneId == channelId, 디스패치 계약).
 *          AABB 모드에서는 겹치면 선언 순서상 먼저가 이기며, 어느 상자에도 안 들면 zoneId = -1.
 */
struct SpatialZone {
    veda::ChannelId zoneId = -1;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

/**
 * @brief   도면 공통 월드 좌표의 유효 범위 (로컬→월드 변환 결과 sanity check)
 * @details compute-server 의 localBounds 와 같은 역할. cameraPosX/Y 오타 같은 캘리브레이션
 *          오류로 도면 밖에 사상된 객체를 걸러낸다 (그냥 두면 zone/위험 판정이 조용히 틀어짐)
 * @note    enabled=false 면 검사하지 않음 (기본값)
 */
struct WorldBounds {
    bool enabled = false;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

inline void from_json(const nlohmann::json& j, WorldBounds& b) {
    b.enabled = veda::detail::get_or<bool>(j, "enabled", b.enabled);
    b.minX = veda::detail::get_or<double>(j, "minX", b.minX);
    b.maxX = veda::detail::get_or<double>(j, "maxX", b.maxX);
    b.minY = veda::detail::get_or<double>(j, "minY", b.minY);
    b.maxY = veda::detail::get_or<double>(j, "maxY", b.maxY);
}

struct ParkingPoint {
    double x = 0.0;
    double y = 0.0;
};

inline void from_json(const nlohmann::json& j, ParkingPoint& p) {
    p.x = veda::detail::get_or<double>(j, "x", p.x);
    p.y = veda::detail::get_or<double>(j, "y", p.y);
}

struct ParkingSpace {
    std::vector<ParkingPoint> points;
};

inline void from_json(const nlohmann::json& j, ParkingSpace& s) {
    s.points = veda::detail::get_or<std::vector<ParkingPoint>>(j, "points", s.points);
}

inline constexpr std::size_t kMaxParkingSpaces = 4096;
inline constexpr std::size_t kMaxParkingVertices = 16;

inline bool isValidParkingSpace(const ParkingSpace& space) {
    if (space.points.size() < 3 || space.points.size() > kMaxParkingVertices) {
        return false;
    }

    double areaTwice = 0.0;
    for (std::size_t i = 0, j = space.points.size() - 1; i < space.points.size(); j = i++) {
        const ParkingPoint& a = space.points[j];
        const ParkingPoint& b = space.points[i];
        if (!std::isfinite(a.x) || !std::isfinite(a.y)) {
            return false;
        }
        areaTwice += a.x * b.y - b.x * a.y;
    }
    return std::isfinite(areaTwice) && std::abs(areaTwice) > 1e-9;
}

/** 주차면 내부 차량의 정지 상태 판정 설정. spaces가 비어 있으면 정책은 no-op이다. */
struct ParkingPolicyConfig {
    std::uint64_t stationaryDurationMs = 5000;
    std::uint64_t maxObservationGapMs = 1000;
    double movementToleranceM = 0.3;
    std::vector<ParkingSpace> spaces;
};

inline void from_json(const nlohmann::json& j, ParkingPolicyConfig& p) {
    p.stationaryDurationMs =
        veda::detail::get_or<std::uint64_t>(j, "stationaryDurationMs", p.stationaryDurationMs);
    p.maxObservationGapMs =
        veda::detail::get_or<std::uint64_t>(j, "maxObservationGapMs", p.maxObservationGapMs);
    p.movementToleranceM = veda::detail::get_or<double>(j, "movementToleranceM", p.movementToleranceM);
    p.spaces = veda::detail::get_or<std::vector<ParkingSpace>>(j, "spaces", p.spaces);
}

inline void from_json(const nlohmann::json& j, SpatialZone& z) {
    z.zoneId = veda::detail::get_or<veda::ChannelId>(j, "zoneId", -1);
    z.minX = veda::detail::get_or<double>(j, "minX", 0.0);
    z.maxX = veda::detail::get_or<double>(j, "maxX", 0.0);
    z.minY = veda::detail::get_or<double>(j, "minY", 0.0);
    z.maxY = veda::detail::get_or<double>(j, "maxY", 0.0);
}

/**
 * @brief STM32 하트비트 및 명령-상태 불일치 대응 정책 (§3-C, §3-B)
 */
struct HwHealthCheckConfig {
    uint32_t heartbeatIntervalMs = 500;        ///< STM32 → RPi 상태 보고 주기
    uint32_t missedBeatsForTimeout = 3;        ///< 연속 유실 시 채널 dead 판정 기준
    uint32_t mismatchRetryCount = 2;           ///< 명령-실제상태 불일치 시 재전송 횟수
    bool mismatchEscalateAfterRetries = true;  ///< 재시도 소진 시 대시보드 fault 표시 여부

    /// @brief STM32가 연결된 시리얼 장치 경로
    std::string devicePath = "/dev/serial0";
};

inline void from_json(const nlohmann::json& j, HwHealthCheckConfig& h) {
    h.heartbeatIntervalMs = veda::detail::get_or<uint32_t>(j, "heartbeatIntervalMs", h.heartbeatIntervalMs);
    h.missedBeatsForTimeout = veda::detail::get_or<uint32_t>(j, "missedBeatsForTimeout", h.missedBeatsForTimeout);
    h.mismatchRetryCount = veda::detail::get_or<uint32_t>(j, "mismatchRetryCount", h.mismatchRetryCount);
    h.mismatchEscalateAfterRetries =
        veda::detail::get_or<bool>(j, "mismatchEscalateAfterRetries", h.mismatchEscalateAfterRetries);
    h.devicePath = veda::detail::get_or<std::string>(j, "devicePath", h.devicePath);
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
 *                  월드 좌표계 atan2 규약으로는 normalize(90 - facingAngleDeg)로 변환
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

/** 모든 zone이 물리 CCTV당 4개 방향 채널로 구성됐는지 확인한다. */
inline bool supportsDirectionalZoneMapping(const std::vector<SpatialZone>& zones,
                                           const std::vector<CameraCalibration>& calibrations) {
    if (zones.empty() || zones.size() % 4 != 0) {
        return false;
    }

    std::vector<const CameraCalibration*> matched;
    matched.reserve(zones.size());
    for (std::size_t zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
        const SpatialZone& zone = zones[zoneIndex];
        const double width = zone.maxX - zone.minX;
        const double height = zone.maxY - zone.minY;
        if (!std::isfinite(zone.minX) || !std::isfinite(zone.maxX) || !std::isfinite(zone.minY) ||
            !std::isfinite(zone.maxY) || !(width > 0.0) || !(height > 0.0) || std::abs(width - height) > 1e-9) {
            return false;
        }
        for (std::size_t previous = 0; previous < zoneIndex; ++previous) {
            if (zones[previous].zoneId == zone.zoneId) {
                return false;
            }
        }

        const CameraCalibration* calibration = nullptr;
        for (const CameraCalibration& candidate : calibrations) {
            if (candidate.channelId == zone.zoneId) {
                if (calibration != nullptr) {
                    return false;
                }
                calibration = &candidate;
            }
        }
        if (calibration == nullptr || !std::isfinite(calibration->cameraPosX) ||
            !std::isfinite(calibration->cameraPosY) || !std::isfinite(calibration->facingAngleDeg)) {
            return false;
        }
        matched.push_back(calibration);
    }

    for (std::size_t zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
        const CameraCalibration* calibration = matched[zoneIndex];
        std::size_t channelCount = 0;
        for (std::size_t candidateIndex = 0; candidateIndex < matched.size(); ++candidateIndex) {
            const CameraCalibration* candidate = matched[candidateIndex];
            if (candidate->cameraPosX == calibration->cameraPosX &&
                candidate->cameraPosY == calibration->cameraPosY) {
                const SpatialZone& candidateZone = zones[candidateIndex];
                if (candidateZone.minX != zones[zoneIndex].minX || candidateZone.maxX != zones[zoneIndex].maxX ||
                    candidateZone.minY != zones[zoneIndex].minY || candidateZone.maxY != zones[zoneIndex].maxY) {
                    return false;
                }
                ++channelCount;
            }
        }
        if (channelCount != 4) {
            return false;
        }
    }
    return true;
}

struct AppConfig {
    // [파이프라인 설정]
    /**
     * @brief   프레임 집계 시간 윈도우 (ms)
     *
     * @details
     * 소스 프레임 주기(T_src, compute-server 실측 4.96fps = 201.6ms)보다 **커야** 한다.
     * 작으면 윈도우 하나에 전체 채널이 다 들어오지 못해 채널 조각화가 생긴다
     * (100ms 에서는 윈도우당 평균 2/4 채널만 잡혔고, 위상이 프레임당 1.6ms 씩 밀리며
     *  어느 두 채널이 잡히는지가 약 12.5초 주기로 순환했다).
     *
     * 200 이 아니라 220 인 이유: 200 은 T_src(201.6ms) 와 사실상 같아 드리프트에 따라
     * 어떤 윈도우는 2프레임, 어떤 윈도우는 0프레임을 받는 최악의 경계다.
     *
     * @warning 이 값을 바꾸면 RiskConfig::trackMaxDistance 를 반드시 함께 재계산할 것.
     *          추적 가능 최대 속도 = trackMaxDistance / max(windowSizeMs, T_src) 다.
     */
    uint64_t windowSizeMs = 220;
    int channelCount = 4;  ///< 채널(zone) 개수

    /**
     * @brief   [데모 전용] 수신한 Human 을 Vehicle 로 치환할지 여부
     *
     * @details
     * 실내 축소 데모에서 사람이 차량 대역을 대신 연기하기 위한 스위치다. 위험 판정은 차량
     * 중심이라(ThresholdRiskPolicy 원칙 1: 차량이 없으면 전부 None) 사람만 걸어다니면
     * 경보가 하나도 울리지 않는다. 그렇다고 판정 규칙을 데모용으로 고치면 시연한 것과
     * 배포하는 것이 달라지므로, **핵심 로직은 그대로 두고 ingest 최외곽에서 cls 만 바꾼다.**
     * 치환 지점은 MqttChannelReceiver::processMessage — 디코드/검증 직후, 집계기에 넣기 전이다.
     *
     * @warning 운영 배포에서는 반드시 false. true 면 실제 보행자가 전부 차량으로 판정되어
     *          "사람 옆의 사람"이 차량 근접 경보를 울린다. 켜져 있으면 start() 가 에러 레벨로
     *          경고를 남기므로 로그에서 바로 확인할 수 있다.
     */
    bool demoPedestrianProxy = false;

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

    /// @brief 로그 CSV 파일 이름 (작업 디렉터리 기준). 회전/압축/보존은 logrotate 가 담당하므로
    ///        반드시 고정 이름이어야 함 (날짜별 이름을 쓰면 logrotate 와 이중 회전이 됨)
    std::string logFileName = "veda.csv";
    /** @} */

    // [위험도 정책 설정]
    RiskConfig risk;

    // [zone 배정 설정 — 전역 도면상의 공간 경계 상자]
    /**
     * @brief   객체를 하드웨어 zone(액추에이터 채널)에 배정하는 zone 목록
     *
     * @note 4방향 구성은 zoneId 0~3을 각각 한 번씩 선언한다. 그 외 구성의 공간 상자는
     *       본질적으로 현장 도면 실측값이라 의미 있는 범용 기본값이 없다.
     *       비워 두면 모든 객체가 zoneId=-1(미배정)로 남아 알람이 울리지 않으므로(안전한 실패),
     *       실제 배포에서는 반드시 config.json 의 "zones" 로 채울 것. SpatialZone 참고.
     */
    std::vector<SpatialZone> zones;

    /// 최근접 물리 CCTV 선택 후 해당 CCTV의 방향 채널을 배정한다.
    bool directionalZoneMapping = false;

    /**
     * @brief   zone 경계 히스테리시스 여유 폭 (m). 0 이면 히스테리시스 비활성
     *
     * @details
     * 직전 프레임의 zone 을 이 폭만큼 넓힌 상자 안에 있으면 zoneId 를 그대로 유지한다.
     * 경계에 걸친 객체가 좌표 잡음만으로 zoneId 를 매 프레임 뒤집는 1프레임 진동을 막는데,
     * 그 진동은 하류에서 두 채널의 알람이 번갈아 켜지는 것으로 나타난다.
     *
     * @warning [ zone 크기와의 결합 ] 이 값은 zone 크기에 비해 충분히 작아야 한다.
     *          가장 작은 zone 의 절반 변보다 커지면 넓힌 상자가 이웃 zone 을 통째로 덮어
     *          객체가 처음 배정된 zone 에서 영영 못 빠져나온다 (경보가 엉뚱한 채널에 고정됨).
     *          축소 스케일 배치에서 zones 만 줄이고 이 값을 그대로 두는 것이 전형적인 실수라
     *          load() 가 그 경우를 경고한다.
     */
    double hysteresisMargin = 0.5;

    // [좌표 변환 설정 — 채널별 카메라 캘리브레이션]
    std::vector<CameraCalibration> cameraCalibrations;

    // [좌표 변환 결과 유효 범위 — 캘리브레이션 오류로 도면 밖에 사상된 객체를 폐기]
    WorldBounds worldBounds;

    // [주차 차량 제외 정책 — 주차면 내부에서 일정 시간 정지한 Vehicle만 제거]
    ParkingPolicyConfig parking;

    // [HW 헬스체크 설정]
    HwHealthCheckConfig hwHealthCheck;

    // [테스트 시뮬레이션 설정 — NullReceiver 전용]
    SimulationConfig simulation;

    // [네트워크 설정]
    std::string mqttBrokerUrl;  ///< config.json에서 주입 (예: tcp://host:1883, mqtts://host:8883)
    /**
     * @brief   RiskFrame 발행 토픽
     * @warning 기본값이 계약(veda::topic::kRisk = "veda/risk")과 달라서, 이대로 두면
     *          클라이언트가 구독하는 토픽으로 나가지 않음 -> 계약값으로 맞춤
     *          다른 값을 넣으면 Contract.h 를 어기는 것이므로 클라이언트도 함께 바꿔야 함
     */
    std::string mqttSendTopic = veda::topic::kRisk;
    std::string mqttCaFile;    ///< mqttBrokerUrl이 ssl/mqtts일 때 config.json에서 주입하는 TLS CA 경로
    std::string mqttClientId;  ///< 비어있으면 MqttTransport가 자동 생성
    int mqttKeepAliveSeconds = 60;
    int mqttReconnectDelaySeconds = 1;      ///< 재연결 대기 시간 초기값 (초)
    int mqttReconnectDelayMaxSeconds = 10;  ///< 재연결 대기 시간 상한 (초, 지수 백오프)

    /// @brief MqttChannelReceiver가 최초 구독/연결에 실패했을 때 재시도하는 간격 (ms)
    uint64_t mqttReceiverRetryIntervalMs = 2000;

    /** 위험 판정에 필수인 zone과 채널별 카메라 보정값이 완전한지 검사한다. */
    void validateForStartup() const {
        if (channelCount < 1 || zones.size() != static_cast<std::size_t>(channelCount)) {
            throw std::invalid_argument("zones must contain exactly one entry for every channel");
        }

        std::vector<bool> zoneSeen(static_cast<std::size_t>(channelCount), false);
        for (const SpatialZone& zone : zones) {
            if (zone.zoneId < 0 || zone.zoneId >= channelCount || zoneSeen[static_cast<std::size_t>(zone.zoneId)]) {
                throw std::invalid_argument("zones must contain unique IDs in [0, channelCount)");
            }
            zoneSeen[static_cast<std::size_t>(zone.zoneId)] = true;
        }

        std::vector<bool> calibrationSeen(static_cast<std::size_t>(channelCount), false);
        for (const CameraCalibration& calibration : cameraCalibrations) {
            if (calibration.channelId < 0 || calibration.channelId >= channelCount ||
                calibrationSeen[static_cast<std::size_t>(calibration.channelId)]) {
                throw std::invalid_argument("cameraCalibrations must contain unique IDs in [0, channelCount)");
            }
            if (!std::isfinite(calibration.cameraPosX) || !std::isfinite(calibration.cameraPosY) ||
                !std::isfinite(calibration.facingAngleDeg) ||
                (calibration.lateralSign != -1 && calibration.lateralSign != 1)) {
                throw std::invalid_argument(
                    "camera calibration values must be finite and lateralSign must be -1 or 1");
            }
            calibrationSeen[static_cast<std::size_t>(calibration.channelId)] = true;
        }

        if (std::find(calibrationSeen.begin(), calibrationSeen.end(), false) != calibrationSeen.end()) {
            throw std::invalid_argument("cameraCalibrations must contain exactly one entry for every channel");
        }
        if (directionalZoneMapping && !supportsDirectionalZoneMapping(zones, cameraCalibrations)) {
            throw std::invalid_argument("directional zone mapping requires four calibrated channels per CCTV");
        }
        if (parking.stationaryDurationMs == 0 || parking.maxObservationGapMs == 0 ||
            !std::isfinite(parking.movementToleranceM) || parking.movementToleranceM < 0.0 ||
            parking.spaces.size() > kMaxParkingSpaces) {
            throw std::invalid_argument("invalid parking policy configuration");
        }
        for (const ParkingSpace& space : parking.spaces) {
            if (!isValidParkingSpace(space)) {
                throw std::invalid_argument("parking space must be a finite, non-degenerate polygon");
            }
        }
    }

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
        constexpr int kMaxChannelCount = static_cast<int>(std::numeric_limits<uint8_t>::max()) + 1;
        if (config.channelCount < 1 || config.channelCount > kMaxChannelCount) {
            const int rawChannelCount = config.channelCount;
            config.channelCount = (config.channelCount < 1) ? 1 : kMaxChannelCount;
            std::cerr << "[Config] 경고: channelCount=" << rawChannelCount << " 는 하드웨어 채널 범위 [1, "
                      << kMaxChannelCount << "] 밖입니다 — " << config.channelCount << " 로 보정합니다.\n";
        }

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
        config.logFileName = veda::detail::get_or<std::string>(j, "logFileName", config.logFileName);
        if (config.logFileName.empty()) {
            std::cerr << "[Config] 경고: logFileName 이 비어 있습니다 — 기본값(veda.csv)을 사용합니다.\n";
            config.logFileName = "veda.csv";
        }
        config.risk = veda::detail::get_or<RiskConfig>(j, "risk", config.risk);
        if (!std::isfinite(config.risk.positionJitterRadius) || config.risk.positionJitterRadius < 0.0) {
            std::cerr << "[Config] 경고: positionJitterRadius=" << config.risk.positionJitterRadius
                      << " 는 0 이상의 유한한 값이어야 합니다 — 0(비활성화)으로 보정합니다.\n";
            config.risk.positionJitterRadius = 0.0;
        }
        config.demoPedestrianProxy = veda::detail::get_or<bool>(j, "demoPedestrianProxy", config.demoPedestrianProxy);
        config.zones = veda::detail::get_or<std::vector<SpatialZone>>(j, "zones", config.zones);
        bool legacyFourDirection = config.zones.size() == 4;
        std::vector<bool> seenLegacyIds(4, false);
        for (const SpatialZone& zone : config.zones) {
            if (zone.zoneId < 0 || zone.zoneId >= 4 || seenLegacyIds[static_cast<std::size_t>(zone.zoneId)]) {
                legacyFourDirection = false;
                break;
            }
            seenLegacyIds[static_cast<std::size_t>(zone.zoneId)] = true;
        }
        config.directionalZoneMapping =
            veda::detail::get_or<bool>(j, "directionalZoneMapping", legacyFourDirection);
        config.hysteresisMargin = veda::detail::get_or<double>(j, "hysteresisMargin", config.hysteresisMargin);
        config.cameraCalibrations =
            veda::detail::get_or<std::vector<CameraCalibration>>(j, "cameraCalibrations", config.cameraCalibrations);
        config.worldBounds = veda::detail::get_or<WorldBounds>(j, "worldBounds", config.worldBounds);
        config.parking = veda::detail::get_or<ParkingPolicyConfig>(j, "parking", config.parking);

        bool directionalZones = config.directionalZoneMapping;
        if (directionalZones && !legacyFourDirection &&
            !supportsDirectionalZoneMapping(config.zones, config.cameraCalibrations)) {
            std::cerr << "[Config] 경고: directionalZoneMapping에는 물리 CCTV당 동일 위치의 4개 채널과 "
                         "채널별 캘리브레이션이 필요합니다 — 시작 검증에서 거부됩니다.\n";
        }

        // [zone 값 검증] zoneId == channelId 이므로 하드웨어 채널 범위를 벗어난 항목을 통과시키면
        // 해당 구역은 위험도 집계에서 제외되어 알람이 울리지 않는다. 다른 채널로 clamp 하면
        // 엉뚱한 액추에이터가 동작하므로, 잘못된 항목은 경고 후 제거한다.
        std::vector<SpatialZone> validZones;
        validZones.reserve(config.zones.size());
        std::vector<bool> seenZoneIds(static_cast<std::size_t>(config.channelCount), false);
        for (std::size_t i = 0; i < config.zones.size(); ++i) {
            const SpatialZone& zone = config.zones[i];
            if (zone.zoneId < 0 || zone.zoneId >= config.channelCount) {
                std::cerr << "[Config] 경고: zones[" << i << "].zoneId=" << zone.zoneId << " 가 범위 [0, "
                          << config.channelCount << ") 밖입니다 — 이 항목을 제거합니다.\n";
                continue;
            }
            if (!directionalZones &&
                (!std::isfinite(zone.minX) || !std::isfinite(zone.maxX) || !std::isfinite(zone.minY) ||
                 !std::isfinite(zone.maxY) || zone.minX > zone.maxX || zone.minY > zone.maxY)) {
                std::cerr << "[Config] 경고: zones[" << i
                          << "] 의 좌표 범위가 유효하지 않습니다 — 이 항목을 제거합니다.\n";
                continue;
            }

            const auto zoneIndex = static_cast<std::size_t>(zone.zoneId);
            if (seenZoneIds[zoneIndex]) {
                std::cerr << "[Config] 경고: zones 에 zoneId=" << zone.zoneId
                          << " 항목이 중복되었습니다 — 첫 번째 항목만 사용합니다.\n";
                continue;
            }

            seenZoneIds[zoneIndex] = true;
            validZones.push_back(zone);
        }
        config.zones.swap(validZones);

        // [히스테리시스 여유 폭 검증] SpatialZoneMapper 의 생성자는 비유한/음수를 예외로 거부하지만
        // AppConfig 는 절대 던지지 않는 계약이므로 여기서 먼저 보정한다 (생성자 검사는 DI/테스트 경로용
        // 이중 방어로 남는다). 잘못된 값에서 0(비활성)이 아니라 기본값으로 되돌리는 것은 의도적이다
        // -- 히스테리시스를 조용히 꺼 버리면 두 채널 알람이 번갈아 켜지는 하드웨어 오동작이 되살아난다.
        if (!std::isfinite(config.hysteresisMargin) || config.hysteresisMargin < 0.0) {
            std::cerr << "[Config] 경고: hysteresisMargin=" << config.hysteresisMargin
                      << " 는 0 이상의 유한한 값이어야 합니다 — 기본값(0.5)으로 보정합니다.\n";
            config.hysteresisMargin = 0.5;
        }

        // 넓힌 상자가 이웃 zone 을 삼키면 객체가 첫 zone 에 영구히 갇힌다. 축소 스케일 배치에서
        // zones 만 줄이고 이 값을 그대로 두는 실수가 흔해 경고만 남긴다 (보정하면 운영자 의도를
        // 덮어쓰게 되고, 올바른 값은 현장 도면에 달려 있어 여기서 정할 수 없다).
        if (!directionalZones && config.hysteresisMargin > 0.0 && !config.zones.empty()) {
            double smallestHalfExtent = std::numeric_limits<double>::max();
            for (const SpatialZone& zone : config.zones) {
                smallestHalfExtent =
                    std::min({smallestHalfExtent, (zone.maxX - zone.minX) * 0.5, (zone.maxY - zone.minY) * 0.5});
            }
            if (config.hysteresisMargin > smallestHalfExtent) {
                std::cerr << "[Config] 경고: hysteresisMargin(" << config.hysteresisMargin
                          << ") 이 가장 작은 zone 의 절반 변(" << smallestHalfExtent
                          << ") 보다 큽니다 — 객체가 처음 배정된 zone 에서 빠져나오지 못할 수 있습니다.\n";
            }
        }

        if (config.worldBounds.enabled && (config.worldBounds.maxX <= config.worldBounds.minX ||
                                           config.worldBounds.maxY <= config.worldBounds.minY)) {
            std::cerr << "[Config] 경고: worldBounds 범위가 비어 있습니다 (max <= min) — 범위 검사를 끕니다.\n";
            config.worldBounds.enabled = false;
        }

        // [캘리브레이션 값 검증] AppConfig 는 예외를 던지지 않으므로 경고 + 보정만 한다.
        // 값이 조용히 잘못되면 좌표가 통째로 틀어지는데도 아무 증상이 없어서 추적이 매우 어렵다
        for (std::size_t i = 0; i < config.cameraCalibrations.size(); ++i) {
            CameraCalibration& cal = config.cameraCalibrations[i];

            // 방위각을 [0,360) 으로 정규화 (음수/360 초과 입력 허용)
            const double rawFacing = cal.facingAngleDeg;
            cal.facingAngleDeg = std::fmod(cal.facingAngleDeg, 360.0);
            if (cal.facingAngleDeg < 0.0)
                cal.facingAngleDeg += 360.0;
            if (std::abs(rawFacing - cal.facingAngleDeg) > 1e-9) {
                std::cerr << "[Config] 경고: 채널 " << cal.channelId << " facingAngleDeg=" << rawFacing
                          << " 를 [0,360) 으로 정규화했습니다 -> " << cal.facingAngleDeg << "\n";
            }

            // lateralSign 은 +1 / -1 만 유효. 0 이나 2 같은 오타를 통과시키면 좌우가 조용히 뒤집힘
            if (cal.lateralSign != 1 && cal.lateralSign != -1) {
                const int corrected = (cal.lateralSign > 0) ? 1 : -1;
                std::cerr << "[Config] 경고: 채널 " << cal.channelId << " lateralSign=" << cal.lateralSign
                          << " 은 +1 또는 -1 이어야 합니다 — " << corrected << " 로 보정합니다.\n";
                cal.lateralSign = corrected;
            }

            if (cal.channelId < 0 || cal.channelId >= config.channelCount) {
                std::cerr << "[Config] 경고: cameraCalibrations 의 channelId=" << cal.channelId << " 가 범위 [0, "
                          << config.channelCount << ") 밖입니다 — 이 항목은 사용되지 않습니다.\n";
            }

            for (std::size_t k = 0; k < i; ++k) {
                if (config.cameraCalibrations[k].channelId == cal.channelId) {
                    std::cerr << "[Config] 경고: cameraCalibrations 에 channelId=" << cal.channelId
                              << " 항목이 중복되었습니다 — 첫 번째 항목만 사용됩니다.\n";
                    break;
                }
            }
        }
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
                          << "필수 캘리브레이션 누락으로 시작 검증에서 거부됨\n";
            }
        }

        return config;
    }
};
