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
 * @brief 월드 좌표 (모든 채널이 같은 프레임을 씀)
 *
 * @details
 * - 단위 : meter
 * - 원점 : 도면상 지정한 지면 위의 CCTV 위치
 * - +x  : 도면 기준 오른쪽
 * - +y  : 도면 기준 위쪽
 * - z   : 없음
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
    WorldPoint pos;                          ///< 지면 접촉점을 호모그래피로 사상한 결과
    bool edge = false;                       ///< bbox가 화면 경계에 닿았음
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

}  // namespace topic

/**
 * @namespace   qos
 * @brief       MQTT QoS 설정값
 */
namespace qos {
inline constexpr int kTopView = 0;  ///< TopView 스트림용 QoS
inline constexpr int kRisk = 1;     ///< Risk 이벤트용 QoS
inline constexpr int kAlive = 1;    ///< LWT 용 QoS
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
    o.pos = detail::get_or<WorldPoint>(j, "pos", WorldPoint{});
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

}  // namespace veda

namespace veda::topic {

inline std::string blur(ChannelId ch) {
    return "veda/ch/" + std::to_string(ch) + "/blur";
}

inline constexpr auto kBlurAll = "veda/ch/+/blur";

}  // namespace veda::topic

namespace veda::qos {

inline constexpr int kBlur = 0;

}  // namespace veda::qos