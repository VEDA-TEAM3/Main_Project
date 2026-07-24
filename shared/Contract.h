#pragma once

/**
 * @file    Contract.h
 * @brief   공유 wire contract
 *
 * @details
 * 필드를 추가/변경하면 kSchemaVersion 을 올리고 세 프로젝트를 함께 빌드
 *
 * @note STM32 프로토콜은 여기 없음-> shared/driver_protocol.h (순수 C, UART)
 * @note 의존성: nlohmann/json >= 3.11
 */

#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace veda {

/**
 * @brief   Schema Version
 * @details 수신 측은 v 가 다르면 경고 로그를 남기고 메시지를 버림
 */
inline constexpr int kSchemaVersion = 1;

/**
 * @name 시간
 * @{
 */

/**
 * @brief TimeStamp (UTC epoch ms)
 * @details
 * 출처는 언제나 카메라의 ONVIF <tt:Frame UtcTime="...">.
 * compute-server의 IParser의 구현체가 파싱 시점에 단 한 번 int64 로 변환하고,
 * 그 값이 파이프라인 전 구간에서 사용
 */
using TimestampMs = std::int64_t;

/// @brief CCTV Channel (0..3)
using ChannelId = int;

/// @brief      ONVIF ObjectId
/// @details    채널 사이에서는 유일하지 않음
using ObjectId = std::int64_t;

/// @brief      control-server의 CrossChannelFuser가 부여하는 전역 ID
/// @details    여러 채널에 잡힌 같은 실체가 하나의 GlobalId 를 갖음
using GlobalId = std::int64_t;

/** @} */

/**
 * @name 좌표계
 * @{
 */

/**
 * @brief 카메라 로컬 지면 좌표 (채널마다 원점과 축 방향이 다름)
 *
 * @details
 * - 단위 : meter
 * - 원점 : 해당 채널 카메라의 설치 위치
 * - +y  : 그 카메라의 전방
 * - +x  : 전방 기준 좌우 오프셋 (어느 쪽이 +인지는 control-server 의
 *         CameraCalibration::lateralSign 이 정함)
 * - z   : 없음
 *
 * @warning [ WorldPoint 와 헷갈리지 말 것 ]
 * compute-server 는 도면을 전혀 모른다. 호모그래피는 '카메라 로컬 지면 평면'에서
 * 캘리브레이션되며, 여기서 나온 좌표를 도면 공통 좌표로 옮기는 것은 control-server 의
 * ILocalToWorldTransform 책임이다
 * -- 즉 호모그래피를 도면 좌표로 캘리브레이션하면 control-server 가 한 번 더 회전시켜
 *    예외도 경고도 없이 완전히 틀린 위치가 나온다
 *
 * @note
 * 예전에는 이 자리에 WorldPoint 를 그대로 썼고 문서도 "모든 채널이 같은 프레임"이라고
 * 적혀 있었음 -- 실제 파이프라인(ILocalToWorldTransform)과 모순이라 타입을 분리함
 * 직렬화 형식은 {"x","y"} 로 WorldPoint 와 동일하므로 wire 호환성은 그대로임
 */
struct LocalPoint {
    double x = 0.0;  ///< meters, 전방 기준 좌우 오프셋
    double y = 0.0;  ///< meters, 카메라 전방 거리
};

/**
 * @brief 도면 공통 월드 좌표 (모든 채널이 같은 프레임을 씀)
 *
 * @details
 * - 단위 : meter
 * - 원점 : 도면상의 기준점 (control-server 설정 기준 = 사거리 중심)
 * - +x  : 도면 기준 오른쪽 (동)
 * - +y  : 도면 기준 위쪽 (북)
 * - z   : 없음
 *
 * @note LocalPoint 를 CameraCalibration(설치 위치 + 방위각)으로 회전·평행이동한 결과
 */
struct WorldPoint {
    double x = 0.0;  ///< meters
    double y = 0.0;  ///< meters
};

/**
 * @brief 원본 영상 프레임 기준의 정규화 사각형
 *
 * @details
 * - 범위 : [0, 1]
 * - 원점 : 좌상단, +t 가 아래쪽 (화면 좌표계)
 *
 * @note
 * ONVIF 원본은 [-1,1], 원점 중앙, +y 가 위쪽
 * IParser의 구현체가 스트림의 <tt:Transformation> 을 읽어 이 형식으로 변환
 */
struct NormRect {
    double l = 0.0;  ///< Left
    double t = 0.0;  ///< Top
    double r = 0.0;  ///< Right
    double b = 0.0;  ///< Bottom
};

/** @} */

/**
 * @name 분류
 * @{
 */

/**
 * @brief 객체 클래스 분류 (ONVIF <tt:Class><tt:Type> 기준)
 */
enum class ObjectClass {
    Unknown = 0,
    Human,         ///< 위험 경로 (지면 접촉점 -> 월드 좌표)
    Vehicle,       ///< 위험 경로
    Head,          ///< 블러 경로 (항상 Human의 자식)
    LicensePlate,  ///< 블러 경로 (항상 Vehicle의 자식)
};

/**
 * @brief   위험 분류인지 확인
 * @details 라우팅 규칙: Parent 없음 + Human|Vehicle -> RISK
 */
inline constexpr bool isRiskClass(ObjectClass c) { return c == ObjectClass::Human || c == ObjectClass::Vehicle; }

/**
 * @brief   블러 분류인지 확인
 * @details 라우팅 규칙: Parent 있음 -> BLUR (Head, LicensePlate)
 */
inline constexpr bool isBlurClass(ObjectClass c) { return c == ObjectClass::Head || c == ObjectClass::LicensePlate; }

/**
 * @brief ObjectClass를 문자열로 변환
 */
inline constexpr std::string_view toString(ObjectClass c) {
    switch (c) {
        case ObjectClass::Human:
            return "Human";
        case ObjectClass::Vehicle:
            return "Vehicle";
        case ObjectClass::Head:
            return "Head";
        case ObjectClass::LicensePlate:
            return "LicensePlate";
        case ObjectClass::Unknown:
            break;
    }
    return "Unknown";
}

/**
 * @brief 문자열을 ObjectClass로 변환
 */
inline ObjectClass objectClassFromString(std::string_view s) {
    if (s == "Human")
        return ObjectClass::Human;
    if (s == "Vehicle")
        return ObjectClass::Vehicle;
    if (s == "Head")
        return ObjectClass::Head;
    if (s == "LicensePlate")
        return ObjectClass::LicensePlate;
    return ObjectClass::Unknown;
}

/** @} */

/**
 * @name 위험 레벨
 * @{
 */

/**
 * @brief 위험 레벨
 */
enum class RiskLevel {
    None = 0,  ///< 사람 감지 X
    Warning,   ///< 사람 감지 or 객체 간 거리 warningDistance 이내
    Danger,    ///< 객체 간 거리 dangerousDistance 이내
};

/// @brief RiskLevel을 문자열로 변환
inline constexpr std::string_view toString(RiskLevel l) {
    switch (l) {
        case RiskLevel::Warning:
            return "Warning";
        case RiskLevel::Danger:
            return "Danger";
        case RiskLevel::None:
            break;
    }
    return "None";
}

/// @brief 문자열을 RiskLevel로 변환
inline RiskLevel riskLevelFromString(std::string_view s) {
    if (s == "Warning")
        return RiskLevel::Warning;
    if (s == "Danger")
        return RiskLevel::Danger;
    return RiskLevel::None;
}

/** @} */

/**
 * @brief Top-View 변환이 완료된 단일 객체 데이터
 */
struct TopViewObject {
    ObjectId id = 0;                         ///< 채널 내 추적 ID
    ObjectClass cls = ObjectClass::Unknown;  ///< Human | Vehicle
    LocalPoint pos;     ///< 지면 접촉점을 호모그래피로 사상한 '카메라 로컬' 좌표
    bool edge = false;  ///< bbox가 화면 경계에 닿았음
};

/**
 * @brief   메시지 1 : TopViewFrame (compute-server -> control-server)
 *
 * @details
 * - 통신: MQTT / TLS, topic::topView(ch), QoS 0
 * - 단위: Frame
 * - 예외: 객체가 하나도 없는 프레임도 보냄 ('빈 프레임'과 '채널 끊김'을 구분)
 */
struct TopViewFrame {
    int v = kSchemaVersion;              ///< 스키마 버전
    TimestampMs ts = 0;                  ///< CCTV 시각
    ChannelId ch = 0;                    ///< 채널 ID
    std::vector<TopViewObject> objects;  ///< 인식된 객체 목록
};

/**
 * @brief   Blur 처리 대상 객체
 *
 * @note
 * - ObjectClass: [Head | LicensePlate] blur 유무를 추가할 수 있으므로 포함
 */
struct BlurTarget {
    ObjectId id = 0;
    ObjectClass cls = ObjectClass::Unknown;  ///< Head | LicensePlate
    NormRect box;                            ///< [0,1], 좌상단 원점
};

/**
 * @brief   메시지 2 : BlurFrame (compute-server -> client)
 * @details
 * - 통신: RTSP/RTP (GStreamer), payload = JSON
 * - 픽셀 없이 좌표만 전송
 * - 동기화: ts로 매칭
 * - Metadata의 RTP 타임스탬프를 동기화에 사용하지 말 것
 */
struct BlurFrame {
    int v = kSchemaVersion;         ///< 스키마 버전
    TimestampMs ts = 0;             ///< CCTV 캡처 시각
    ChannelId ch = 0;               ///< 채널 ID
    std::vector<BlurTarget> blurs;  ///< 블러 대상 목록
};

/**
 * @brief 4채널 융합이 완료된 단일 위험 객체
 */
struct RiskObject {
    GlobalId gid = 0;  ///< 융합 후 전역 ID
    ObjectClass cls = ObjectClass::Unknown;
    WorldPoint pos;                     ///< 월드 좌표
    RiskLevel level = RiskLevel::None;  ///< 이 객체 기준 최고 위험 레벨
    GlobalId nearest = 0;               ///< 최근접 객체의 gid (없으면 0)
    double dist = -1.0;                 ///< 최근접 거리(m) (없으면 음수)
};

/**
 * @struct  RiskFrame
 * @brief   메시지 3 : RiskFrame (control-server -> client)
 * @details
 * - 통신: MQTT / TLS, topic::kRisk, QoS 1
 * - 4채널을 융합한 결과 (앱이 Top-View 디지털 트윈을 그리는 데 사용)
 * - 위험 객체만이 아니라 프레임의 모든 객체를 보냄
 */
struct RiskFrame {
    int v = kSchemaVersion;             ///< 스키마 버전
    TimestampMs ts = 0;                 ///< CCTV 시각
    RiskLevel level = RiskLevel::None;  ///< 프레임 전체의 최고 위험 레벨
    std::vector<RiskObject> objects;    ///< 융합된 객체 목록
};

/**
 * @struct  ChannelStatus
 * @brief   메시지 4 : ChannelStatus (control-server -> client)
 *
 * @details
 * - 통신: MQTT / TLS, topic::hwStatus(ch), QoS 1, retained
 * - RiskFrame과 갱신 성격이 달라 별도 메시지로 분리함:
 *   RiskFrame은 윈도우마다(예: 100ms) 계속 나가는 고빈도 스트림인 반면,
 *   이 메시지는 상태가 실제로 바뀔 때만(가끔) 발행되는 이벤트임
 * - retained로 발행하므로 클라이언트가 재접속해도 최신 상태를 즉시 받음
 *   (compute-server -> control-server의 topic::alive(ch) LWT와 동일한 패턴)
 *
 * @note [ cameraAlive vs hardwareAlive 를 분리한 이유 ]
 * 채널이 죽은 원인을 대시보드에서 구분할 수 있어야 함:
 * - cameraAlive=false  : 그 채널의 compute-server/CCTV 연결이 끊김 (MQTT LWT 기준)
 * - hardwareAlive=false: STM32가 그 채널의 HW(LED/siren/buzzer)에 대해 하트비트 응답 없음
 * 카메라는 살아있는데 STM32만 죽었을 수도, 그 반대일 수도 있어서 하나의 bool로
 * 뭉치면 현장에서 "뭘 고쳐야 하는지" 알 수 없게 됨
 *
 * @note [ v2: sirenOn/buzzerOn/led* 추가 ]
 * 예전에는 hardwareAlive(STM32 응답 여부)만 있어서, 응답은 살아있어도 그 채널 하드웨어가
 * 실제로 경광등/부저/LED 중 무엇을 켜고 있는지는 Qt 클라이언트가 전혀 알 수 없었음
 * -> STM32가 상행(veda_uplink_packet_t, driver_protocol.h)으로 이미 올려보내던
 *    siren_on/buzzer_on/led_red/led_yellow/led_green 을 그대로 실어 보냄.
 *    hardwareAlive=false 인 동안의 값은 마지막으로 확인된 상태일 뿐 최신이 아님(신뢰 X)
 *
 * @warning [ 클라이언트 계약 (STRICT) — 표시 상태는 hardwareAlive 로 게이트할 것 ]
 * sirenOn/buzzerOn/ledRed/ledYellow/ledGreen 은 "지금 이 하드웨어가 무엇을 켜고 있는가"의
 * 무조건적 최신값이 아니다. 반드시 hardwareAlive 와 함께 해석해야 하며 계약은 다음과 같다:
 *  - hardwareAlive=true  : 표시 상태 필드는 유효한 실시간 값 -> 그대로 렌더링 가능
 *  - hardwareAlive=false : 표시 상태 필드는 보드가 죽기 직전 '마지막으로 확인된' 값(=stale)
 *                          -> 절대 활성 상태(예: "사이렌 켜짐")로 렌더링하지 말 것
 *
 * 서버는 이 게이팅을 강제하지 않는다. 마지막 상태를 보존하기 위해(사후 진단용) 원시값을
 * 그대로 싣기로 결정했기 때문이다 -- 따라서 게이팅 책임은 전적으로 클라이언트(Qt)에 있다.
 * 소비자는 표시 상태를 그리기 전에 반드시 hardwareAlive 를 먼저 검사하고, false 이면 개별
 * 표시 대신 '연결 끊김'을 나타내는 UI(예: 채널 타일에 빨간 테두리)를 그려야 한다.
 * 이것은 권고가 아니라 강한 API 계약이다. (상류 계약: IHwEventDispatcher.h::HwIndicatorState)
 */
struct ChannelStatus {
    int v = kSchemaVersion;      ///< 스키마 버전
    ChannelId ch = 0;            ///< 채널 ID
    TimestampMs ts = 0;          ///< 이 상태로 전환된 시각 (control-server 로컬 UTC epoch ms)
    bool cameraAlive = false;    ///< compute-server MQTT LWT 기준 (topic::alive(ch))
    bool hardwareAlive = false;  ///< STM32 UART 하트비트 기준

    /// @name STM32 상행(veda_uplink_packet_t)의 실제 표시 상태 (v2)
    /// @details hardwareAlive=false 인 동안은 마지막으로 확인된 값일 뿐이므로 신뢰하지 말 것
    /// @{
    bool sirenOn = false;
    bool buzzerOn = false;
    bool ledRed = false;
    bool ledYellow = false;
    bool ledGreen = false;
    /** @} */
};

/**
 * @namespace   topic
 * @brief       MQTT topic 정의
 */
namespace topic {

/**
 * @brief   [compute-server -> control-server] Top-View 전송 토픽 (QoS 0)
 * @details 연속 스트림이라 유실 복구 불필요 (QoS 1은 중복만 만듦)
 */
inline std::string topView(ChannelId ch) { return "veda/ch/" + std::to_string(ch) + "/topview"; }

/// @brief 모든 채널의 Top-View 구독을 위한 와일드카드 토픽
inline constexpr auto kTopViewAll = "veda/ch/+/topview";

/// @brief [control-server -> client] 위험 객체 전송 토픽 (QoS 1)
inline constexpr auto kRisk = "veda/risk";

/**
 * @brief 채널별 LWT(Last Will and Testament) 토픽 (QoS 1 + retained)
 * @details payload는 한 바이트: "1" = alive, "0" = dead
 */
inline std::string alive(ChannelId ch) { return "veda/ch/" + std::to_string(ch) + "/alive"; }

/// @brief 모든 채널의 LWT 구독을 위한 와일드카드 토픽
inline constexpr auto kAliveAll = "veda/ch/+/alive";

/**
 * @brief   [control-server -> client] 채널 하드웨어/연결 상태 통지 토픽 (QoS 1, retained)
 * @details compute-server의 topView/alive와 이름이 겹치지 않도록 "veda/hw/ch/" 접두사를 씀
 *          (veda/ch/N/... 는 compute-server -> control-server 방향으로 이미 쓰이고 있음)
 */
inline std::string hwStatus(ChannelId ch) { return "veda/hw/ch/" + std::to_string(ch) + "/status"; }

/// @brief 모든 채널의 하드웨어 상태 구독을 위한 와일드카드 토픽
inline constexpr auto kHwStatusAll = "veda/hw/ch/+/status";

}  // namespace topic

/**
 * @namespace   qos
 * @brief       MQTT QoS 설정값
 */
namespace qos {
inline constexpr int kTopView = 0;   ///< TopView 스트림용 QoS
inline constexpr int kRisk = 1;      ///< Risk 이벤트용 QoS
inline constexpr int kAlive = 1;     ///< LWT 용 QoS
inline constexpr int kHwStatus = 1;  ///< 채널 하드웨어 상태 통지용 QoS
}  // namespace qos

/** @cond INTERNAL_JSON_HELPERS */
namespace detail {
/**
 * @brief   필드가 없으면 fallback(기본값)을 반환하는 안전한 파서
 * @details from_json은 절대 던지지 않음
 */
template <typename T>
T get_or(const nlohmann::json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
        return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}
}  // namespace detail
/** @endcond */

/// @name nlohmann/json 직렬화/역직렬화 함수들
/// @{

inline void to_json(nlohmann::json& j, const WorldPoint& p) { j = nlohmann::json{{"x", p.x}, {"y", p.y}}; }
inline void from_json(const nlohmann::json& j, WorldPoint& p) {
    p.x = detail::get_or<double>(j, "x", 0.0);
    p.y = detail::get_or<double>(j, "y", 0.0);
}

// LocalPoint 는 WorldPoint 와 동일한 {"x","y"} 로 직렬화됨 (타입만 분리, wire 는 그대로)
inline void to_json(nlohmann::json& j, const LocalPoint& p) { j = nlohmann::json{{"x", p.x}, {"y", p.y}}; }
inline void from_json(const nlohmann::json& j, LocalPoint& p) {
    p.x = detail::get_or<double>(j, "x", 0.0);
    p.y = detail::get_or<double>(j, "y", 0.0);
}

inline void to_json(nlohmann::json& j, const NormRect& r) {
    j = nlohmann::json{{"l", r.l}, {"t", r.t}, {"r", r.r}, {"b", r.b}};
}
inline void from_json(const nlohmann::json& j, NormRect& r) {
    r.l = detail::get_or<double>(j, "l", 0.0);
    r.t = detail::get_or<double>(j, "t", 0.0);
    r.r = detail::get_or<double>(j, "r", 0.0);
    r.b = detail::get_or<double>(j, "b", 0.0);
}

inline void to_json(nlohmann::json& j, const TopViewObject& o) {
    j = nlohmann::json{{"id", o.id}, {"cls", std::string(toString(o.cls))}, {"pos", o.pos}, {"edge", o.edge}};
}
inline void from_json(const nlohmann::json& j, TopViewObject& o) {
    o.id = detail::get_or<ObjectId>(j, "id", 0);
    o.cls = objectClassFromString(detail::get_or<std::string>(j, "cls", ""));
    o.pos = detail::get_or<LocalPoint>(j, "pos", LocalPoint{});
    o.edge = detail::get_or<bool>(j, "edge", false);
}

inline void to_json(nlohmann::json& j, const TopViewFrame& f) {
    j = nlohmann::json{{"v", f.v}, {"ts", f.ts}, {"ch", f.ch}, {"objects", f.objects}};
}
inline void from_json(const nlohmann::json& j, TopViewFrame& f) {
    f.v = detail::get_or<int>(j, "v", 0);
    f.ts = detail::get_or<TimestampMs>(j, "ts", 0);
    f.ch = detail::get_or<ChannelId>(j, "ch", 0);
    f.objects = detail::get_or<std::vector<TopViewObject>>(j, "objects", {});
}

inline void to_json(nlohmann::json& j, const BlurTarget& b) {
    j = nlohmann::json{{"id", b.id}, {"cls", std::string(toString(b.cls))}, {"box", b.box}};
}
inline void from_json(const nlohmann::json& j, BlurTarget& b) {
    b.id = detail::get_or<ObjectId>(j, "id", 0);
    b.cls = objectClassFromString(detail::get_or<std::string>(j, "cls", ""));
    b.box = detail::get_or<NormRect>(j, "box", NormRect{});
}

inline void to_json(nlohmann::json& j, const BlurFrame& f) {
    j = nlohmann::json{{"v", f.v}, {"ts", f.ts}, {"ch", f.ch}, {"blurs", f.blurs}};
}
inline void from_json(const nlohmann::json& j, BlurFrame& f) {
    f.v = detail::get_or<int>(j, "v", 0);
    f.ts = detail::get_or<TimestampMs>(j, "ts", 0);
    f.ch = detail::get_or<ChannelId>(j, "ch", 0);
    f.blurs = detail::get_or<std::vector<BlurTarget>>(j, "blurs", {});
}

inline void to_json(nlohmann::json& j, const RiskObject& o) {
    j = nlohmann::json{{"gid", o.gid},         {"cls", std::string(toString(o.cls))},
                       {"pos", o.pos},         {"level", std::string(toString(o.level))},
                       {"nearest", o.nearest}, {"dist", o.dist}};
}
inline void from_json(const nlohmann::json& j, RiskObject& o) {
    o.gid = detail::get_or<GlobalId>(j, "gid", 0);
    o.cls = objectClassFromString(detail::get_or<std::string>(j, "cls", ""));
    o.pos = detail::get_or<WorldPoint>(j, "pos", WorldPoint{});
    o.level = riskLevelFromString(detail::get_or<std::string>(j, "level", ""));
    o.nearest = detail::get_or<GlobalId>(j, "nearest", 0);
    o.dist = detail::get_or<double>(j, "dist", -1.0);
}

inline void to_json(nlohmann::json& j, const RiskFrame& f) {
    j = nlohmann::json{{"v", f.v}, {"ts", f.ts}, {"level", std::string(toString(f.level))}, {"objects", f.objects}};
}
inline void from_json(const nlohmann::json& j, RiskFrame& f) {
    f.v = detail::get_or<int>(j, "v", 0);
    f.ts = detail::get_or<TimestampMs>(j, "ts", 0);
    f.level = riskLevelFromString(detail::get_or<std::string>(j, "level", ""));
    f.objects = detail::get_or<std::vector<RiskObject>>(j, "objects", {});
}

inline void to_json(nlohmann::json& j, const ChannelStatus& s) {
    j = nlohmann::json{{"v", s.v},
                       {"ch", s.ch},
                       {"ts", s.ts},
                       {"cameraAlive", s.cameraAlive},
                       {"hardwareAlive", s.hardwareAlive},
                       {"sirenOn", s.sirenOn},
                       {"buzzerOn", s.buzzerOn},
                       {"ledRed", s.ledRed},
                       {"ledYellow", s.ledYellow},
                       {"ledGreen", s.ledGreen}};
}
inline void from_json(const nlohmann::json& j, ChannelStatus& s) {
    s.v = detail::get_or<int>(j, "v", 0);
    s.ch = detail::get_or<ChannelId>(j, "ch", 0);
    s.ts = detail::get_or<TimestampMs>(j, "ts", 0);
    s.cameraAlive = detail::get_or<bool>(j, "cameraAlive", false);
    s.hardwareAlive = detail::get_or<bool>(j, "hardwareAlive", false);
    s.sirenOn = detail::get_or<bool>(j, "sirenOn", false);
    s.buzzerOn = detail::get_or<bool>(j, "buzzerOn", false);
    s.ledRed = detail::get_or<bool>(j, "ledRed", false);
    s.ledYellow = detail::get_or<bool>(j, "ledYellow", false);
    s.ledGreen = detail::get_or<bool>(j, "ledGreen", false);
}

/// @}

/**
 * @brief   구조체를 JSON 문자열로 직렬화
 * @tparam  T 직렬화할 메시지 구조체 타입
 * @param   msg 구조체 인스턴스
 * @return  std::string 인코딩된 JSON 문자열
 */
template <typename T>
std::string encode(const T& msg) {
    return nlohmann::json(msg).dump();
}

/**
 * @brief       JSON 문자열을 구조체로 역직렬화
 * @details     파싱 실패 시 예외를 던지지 않고 기본 생성된 객체(v == 0)를 돌려줌
 * @tparam      T 역직렬화할 메시지 구조체 타입
 * @param       payload 수신한 JSON 페이로드
 * @return      T 디코딩된 메시지 객체
 */
template <typename T>
T decode(std::string_view payload) {
    try {
        return nlohmann::json::parse(payload).get<T>();
    } catch (...) {
        return T{.v = 0};
    }
}

/**
 * @name    Zero-DOM 직렬화 (고빈도 텔레메트리 hot path 전용)
 * @details
 * nlohmann::json DOM 을 프레임마다 새로 만들어 dump() 하면, 객체 수 N 에 비례하는 노드/문자열
 * 힙 할당이 매 프레임 발생한다(compute-server 발행 hot path 의 지배적 할당원). 대신 재사용
 * std::string 버퍼에 필드를 직접 append 하여 DOM 을 완전히 우회한다 -- warmup 이후 힙 할당 0.
 * 출력은 표준 JSON 이므로 수신 측(from_json / nlohmann)이 그대로 파싱한다.
 *
 * @warning wire 포맷(키 이름/구조)은 위의 to_json 과 반드시 일치시킬 것. 한쪽만 바꾸면 송신은
 *          되지만 수신(from_json 의 get_or)이 조용히 기본값을 읽는다 -- 필드 추가 시 둘 다 갱신.
 * @{
 */
namespace detail {

/// @brief 정수를 out 에 append (std::to_chars, 로케일 무관, 힙 할당 없음)
inline void appendInt(std::string& out, std::int64_t value) {
    char buf[24];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, result.ptr);
}

/// @brief double 을 out 에 append (std::to_chars 최단 왕복 표현, 로케일 무관, 힙 할당 없음)
inline void appendDouble(std::string& out, double value) {
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, result.ptr);
}

}  // namespace detail

/// @brief TopViewFrame 을 재사용 버퍼 out 에 직접 직렬화 (DOM 없음). to_json(TopViewFrame) 과 동일 포맷.
inline void encodeInto(const TopViewFrame& f, std::string& out) {
    out.clear();
    out += "{\"v\":";
    detail::appendInt(out, f.v);
    out += ",\"ts\":";
    detail::appendInt(out, f.ts);
    out += ",\"ch\":";
    detail::appendInt(out, f.ch);
    out += ",\"objects\":[";
    for (std::size_t i = 0; i < f.objects.size(); ++i) {
        const TopViewObject& o = f.objects[i];
        if (i != 0)
            out += ',';
        out += "{\"id\":";
        detail::appendInt(out, o.id);
        out += ",\"cls\":\"";
        out += toString(o.cls);  // string_view, 할당 없음
        out += "\",\"pos\":{\"x\":";
        detail::appendDouble(out, o.pos.x);
        out += ",\"y\":";
        detail::appendDouble(out, o.pos.y);
        out += "},\"edge\":";
        out += o.edge ? "true" : "false";
        out += '}';
    }
    out += "]}";
}

/// @brief BlurFrame 을 재사용 버퍼 out 에 직접 직렬화 (DOM 없음). to_json(BlurFrame) 과 동일 포맷.
inline void encodeInto(const BlurFrame& f, std::string& out) {
    out.clear();
    out += "{\"v\":";
    detail::appendInt(out, f.v);
    out += ",\"ts\":";
    detail::appendInt(out, f.ts);
    out += ",\"ch\":";
    detail::appendInt(out, f.ch);
    out += ",\"blurs\":[";
    for (std::size_t i = 0; i < f.blurs.size(); ++i) {
        const BlurTarget& b = f.blurs[i];
        if (i != 0)
            out += ',';
        out += "{\"id\":";
        detail::appendInt(out, b.id);
        out += ",\"cls\":\"";
        out += toString(b.cls);  // string_view, 할당 없음
        out += "\",\"box\":{\"l\":";
        detail::appendDouble(out, b.box.l);
        out += ",\"t\":";
        detail::appendDouble(out, b.box.t);
        out += ",\"r\":";
        detail::appendDouble(out, b.box.r);
        out += ",\"b\":";
        detail::appendDouble(out, b.box.b);
        out += "}}";
    }
    out += "]}";
}

/// @brief RiskFrame 을 재사용 버퍼 out 에 직접 직렬화 (DOM 없음). to_json(RiskFrame) 과 동일 포맷.
inline void encodeInto(const RiskFrame& f, std::string& out) {
    out.clear();
    out += "{\"v\":";
    detail::appendInt(out, f.v);
    out += ",\"ts\":";
    detail::appendInt(out, f.ts);
    out += ",\"level\":\"";
    out += toString(f.level);  // string_view, 할당 없음
    out += "\",\"objects\":[";
    for (std::size_t i = 0; i < f.objects.size(); ++i) {
        const RiskObject& o = f.objects[i];
        if (i != 0)
            out += ',';
        out += "{\"gid\":";
        detail::appendInt(out, o.gid);
        out += ",\"cls\":\"";
        out += toString(o.cls);
        out += "\",\"pos\":{\"x\":";
        detail::appendDouble(out, o.pos.x);
        out += ",\"y\":";
        detail::appendDouble(out, o.pos.y);
        out += "},\"level\":\"";
        out += toString(o.level);
        out += "\",\"nearest\":";
        detail::appendInt(out, o.nearest);
        out += ",\"dist\":";
        detail::appendDouble(out, o.dist);
        out += '}';
    }
    out += "]}";
}

/** @} */

}  // namespace veda

namespace veda::topic {

inline std::string blur(ChannelId ch) { return "veda/ch/" + std::to_string(ch) + "/blur"; }

inline constexpr auto kBlurAll = "veda/ch/+/blur";

}  // namespace veda::topic

namespace veda::qos {

inline constexpr int kBlur = 0;

}  // namespace veda::qos