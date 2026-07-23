#include "parser/OnvifParser.h"

#include <charconv>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Parser";

/// @brief 인식 안 되는 <tt:Type> 문자열 진단 로그 rate-limit용 (파서는 채널당 단일 스레드에서만 호출됨)
std::uint64_t g_unknownTypeCount = 0;

/**
 * @brief   안전한 숫자 파싱을 위한 std::from_chars 래퍼 함수
 * @tparam  T 파싱할 숫자의 타입
 * @param   sv 숫자로 변환할 문자열 뷰
 * @return  std::optional<T> 성공 시 파싱된 값, 실패 시 nullopt
 */
template <typename T>
std::optional<T> parseNumber(std::string_view sv) {
    T value{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{})
        return std::nullopt;
    return value;
}

/**
 * @brief   key="value" 패턴에서 value 를 추출
 * @param   s 검색을 수행할 대상 문자열 (내부에서 시작 위치를 결정)
 * @param   key 찾고자 하는 속성의 키 문자열
 * @return  std::optional<std::string_view> 추출된 따옴표 안의 문자열, 실패 시 nullopt
 *
 * @note    "key" + "=\"" 를 이어붙인 임시 std::string을 만들어 검색하던 방식 대신,
 *          key만 먼저 찾고 바로 뒤가 ="인지 직접 확인함 -> 프레임당 수십 회 호출되는
 *          지점에서 임시 문자열 생성(및 SSO 범위를 벗어날 경우의 힙 할당)을 없앰
 */
std::optional<std::string_view> extractQuoted(std::string_view s, std::string_view key) {
    std::size_t searchFrom = 0;
    for (;;) {
        const std::size_t keyPos = s.find(key, searchFrom);
        if (keyPos == std::string_view::npos)
            return std::nullopt;

        const std::size_t afterKey = keyPos + key.size();
        if (afterKey + 1 < s.size() && s[afterKey] == '=' && s[afterKey + 1] == '"') {
            const std::size_t valueStart = afterKey + 2;
            const std::size_t valueEnd = s.find('"', valueStart);
            if (valueEnd == std::string_view::npos)
                return std::nullopt;
            return s.substr(valueStart, valueEnd - valueStart);
        }

        searchFrom = keyPos + 1;
    }
}

/**
 * @brief       UtcTime="YYYY-MM-DDTHH:MM:SS.sssZ" 형식의 문자열을 epoch ms 로 변환
 * @details     고정 포맷이라 위치 기반으로 자름
 * @param       utc 변환할 UTC 기준 시각 문자열
 * @return      std::optional<veda::TimestampMs> epoch 기준 밀리초 단위 시간, 실패 시 nullopt
 */
std::optional<veda::TimestampMs> parseUtcTimeMs(std::string_view utc) {
    if (utc.size() < 23)
        return std::nullopt;

    auto year = parseNumber<int>(utc.substr(0, 4));
    auto mon = parseNumber<int>(utc.substr(5, 2));
    auto day = parseNumber<int>(utc.substr(8, 2));
    auto hour = parseNumber<int>(utc.substr(11, 2));
    auto min = parseNumber<int>(utc.substr(14, 2));
    auto sec = parseNumber<int>(utc.substr(17, 2));
    auto ms = utc.size() >= 23 ? parseNumber<int>(utc.substr(20, 3)) : std::optional<int>(0);

    if (!year || !mon || !day || !hour || !min || !sec || !ms)
        return std::nullopt;

    std::tm tm{};
    tm.tm_year = *year - 1900;
    tm.tm_mon = *mon - 1;
    tm.tm_mday = *day;
    tm.tm_hour = *hour;
    tm.tm_min = *min;
    tm.tm_sec = *sec;

    const std::time_t epochSec = timegm(&tm);
    if (epochSec == static_cast<std::time_t>(-1))
        return std::nullopt;

    return static_cast<veda::TimestampMs>(epochSec) * 1000 + *ms;
}

/**
 * @brief ONVIF XML 내 <tt:Transformation> 데이터를 담는 구조체
 */
struct Transformation {
    double translateX = 0.0, translateY = 0.0;
    double scaleX = 1.0, scaleY = 1.0;
};

/**
 * @brief       프레임 문자열에서 Transformation 파라미터(Translate, Scale)를 추출
 * @details     각 태그는 오검색 방지를 위해 다음 태그 시작 전까지의 범위에서만 속성을 찾음
 * @param       frame 파싱할 프레임 문자열 데이터
 * @return      std::optional<Transformation> 추출 성공 시 좌표 변환 정보, 실패 시 nullopt
 */
std::optional<Transformation> parseTransformation(std::string_view frame) {
    const size_t transPos = frame.find("<tt:Transformation");
    if (transPos == std::string_view::npos)
        return std::nullopt;

    const size_t translatePos = frame.find("<tt:Translate", transPos);
    const size_t scalePos = frame.find("<tt:Scale", transPos);
    if (translatePos == std::string_view::npos || scalePos == std::string_view::npos)
        return std::nullopt;

    const size_t translateEnd = frame.find('>', translatePos);
    const size_t scaleEnd = frame.find('>', scalePos);
    if (translateEnd == std::string_view::npos || scaleEnd == std::string_view::npos)
        return std::nullopt;

    const auto translateTag = frame.substr(translatePos, translateEnd - translatePos);
    const auto scaleTag = frame.substr(scalePos, scaleEnd - scalePos);

    auto tx = extractQuoted(translateTag, "x");
    auto ty = extractQuoted(translateTag, "y");
    auto sx = extractQuoted(scaleTag, "x");
    auto sy = extractQuoted(scaleTag, "y");
    if (!tx || !ty || !sx || !sy)
        return std::nullopt;

    auto txv = parseNumber<double>(*tx);
    auto tyv = parseNumber<double>(*ty);
    auto sxv = parseNumber<double>(*sx);
    auto syv = parseNumber<double>(*sy);
    if (!txv || !tyv || !sxv || !syv)
        return std::nullopt;

    return Transformation{*txv, *tyv, *sxv, *syv};
}

/**
 * @brief       ONVIF 정규화 좌표를 domain::NormBox X 좌표계로 변환
 * @param       px ONVIF 정규화 좌표 X ([-1,1], 원점 중앙)
 * @param       t 프레임별 Transformation 정보
 * @return      좌상단 원점 기준 정규화 좌표 [0,1]
 */
double normX(double px, const Transformation& t) { return (t.scaleX * px + t.translateX + 1.0) * 0.5; }

/**
 * @brief   ONVIF 정규화 좌표를 domain::NormBox Y 좌표계로 변환
 * @note    y축이 반전되어 보이는 건 CCTV 설치 방향과 무관한 ONVIF 표준 좌표계
 *          Scale_y 가 음수인 게 정상이며, 손으로 부호를 뒤집지 않고 스트림 값을 그대로 적용
 * @param   py ONVIF 정규화 좌표 Y ([-1,1], y 위쪽)
 * @param   t 프레임별 Transformation 정보
 * @return  좌상단 원점 기준 정규화 좌표 [0,1]
 */
double normY(double py, const Transformation& t) { return (1.0 - (t.scaleY * py + t.translateY)) * 0.5; }

}  // namespace

OnvifParser::OnvifParser(double edgeEpsilon) : edgeEpsilon_(edgeEpsilon) {}

/**
 * @brief   메타데이터 RawPacket을 파싱하여 시스템 처리 단위인 ChannelFrame 으로 변환
 *
 * @details
 * - 페이로드당 <tt:Frame> 이 하나라고 가정하며, 여러 개가 온다면 첫 번째만 처리함
 * - Transformation 파라미터 없이는 좌표를 신뢰할 수 없으므로 파싱을 중단하고 빈 프레임을 반환
 * - ID 없는 객체, 위치(BBox)를 알 수 없는 객체, 분류(Type)가 없는 객체는 라우팅이 불가능하므로 스킵
 * - 신뢰할 수 없는 데이터는 완전히 건너뛴 뒤 실제 Type 속성을 찾음
 *
 * @param   raw 파싱할 원본 메타데이터 바이트 배열이 담긴 패킷
 * @return  domain::ChannelFrame 파싱된 결과 객체 (오류/누락 발생 시 예외 없이 빈 객체를 반환)
 */
domain::ChannelFrame OnvifParser::parse(const domain::RawPacket& raw) {
    domain::ChannelFrame result;
    result.channelId = raw.channelId;

    const std::string_view payload(reinterpret_cast<const char*>(raw.bytes.data()), raw.bytes.size());

    const size_t framePos = payload.find("<tt:Frame");
    if (framePos == std::string_view::npos) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " <tt:Frame> 태그 없음 - 프레임 스킵");
        return result;
    }

    const size_t frameEnd = payload.find("</tt:Frame>", framePos);
    if (frameEnd == std::string_view::npos) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " </tt:Frame> 종료 태그 없음 - 프레임 스킵");
        return result;
    }

    const std::string_view frame = payload.substr(framePos, frameEnd - framePos);

    auto utcAttr = extractQuoted(frame, "UtcTime");
    if (!utcAttr) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " UtcTime 속성 없음 - 프레임 스킵");
        return result;
    }
    auto utcMs = parseUtcTimeMs(*utcAttr);
    if (!utcMs) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " UtcTime 형식 파싱 실패 - 프레임 스킵");
        return result;
    }

    auto transform = parseTransformation(frame);
    if (!transform) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " Transformation 파싱 실패 - 프레임 스킵");
        return result;
    }

    result.utcTime = *utcMs;

    size_t pos = 0;
    while ((pos = frame.find("<tt:Object", pos)) != std::string_view::npos) {
        const size_t objEnd = frame.find("</tt:Object>", pos);
        if (objEnd == std::string_view::npos)
            break;

        const std::string_view obj = frame.substr(pos, objEnd - pos);
        pos = objEnd + 12;  // strlen("</tt:Object>")

        const size_t openTagEnd = obj.find('>');
        const std::string_view openTag = openTagEnd == std::string_view::npos ? obj : obj.substr(0, openTagEnd);

        auto idAttr = extractQuoted(openTag, "ObjectId");
        if (!idAttr)
            continue;
        auto id = parseNumber<veda::ObjectId>(*idAttr);
        if (!id)
            continue;

        domain::DetectedObject det;
        det.id = *id;

        if (auto parentAttr = extractQuoted(openTag, "Parent")) {
            if (auto parentId = parseNumber<veda::ObjectId>(*parentAttr)) {
                det.parentId = *parentId;
            }
        }

        const size_t bboxPos = obj.find("<tt:BoundingBox");
        if (bboxPos == std::string_view::npos)
            continue;
        const size_t bboxEnd = obj.find('>', bboxPos);
        const std::string_view bboxTag =
            bboxEnd == std::string_view::npos ? obj.substr(bboxPos) : obj.substr(bboxPos, bboxEnd - bboxPos);

        auto left = extractQuoted(bboxTag, "left");
        auto top = extractQuoted(bboxTag, "top");
        auto right = extractQuoted(bboxTag, "right");
        auto bottom = extractQuoted(bboxTag, "bottom");
        if (!left || !top || !right || !bottom)
            continue;

        auto l = parseNumber<double>(*left);
        auto t = parseNumber<double>(*top);
        auto r = parseNumber<double>(*right);
        auto b = parseNumber<double>(*bottom);
        if (!l || !t || !r || !b)
            continue;

        det.box.l = normX(*l, *transform);
        det.box.r = normX(*r, *transform);
        det.box.t = normY(*t, *transform);
        det.box.b = normY(*b, *transform);

        // 아래변 잘림은 지면점을 직접 망가뜨리므로 따로 표시 (DetectedObject::bottomTruncated 참고)
        det.bottomTruncated = det.box.b >= 1.0 - edgeEpsilon_;
        det.touchesBorder = det.bottomTruncated || det.box.l <= edgeEpsilon_ || det.box.r >= 1.0 - edgeEpsilon_ ||
                            det.box.t <= edgeEpsilon_;

        size_t searchFrom = 0;
        if (const size_t candEnd = obj.find("</tt:ClassCandidate>"); candEnd != std::string_view::npos) {
            searchFrom = candEnd + 20;  // strlen("</tt:ClassCandidate>")
        }

        const size_t typePos = obj.find("<tt:Type", searchFrom);
        if (typePos == std::string_view::npos)
            continue;

        const size_t typeTextStart = obj.find('>', typePos);
        if (typeTextStart == std::string_view::npos)
            continue;
        const size_t typeTextEnd = obj.find('<', typeTextStart);
        if (typeTextEnd == std::string_view::npos)
            continue;

        const std::string_view typeText = obj.substr(typeTextStart + 1, typeTextEnd - typeTextStart - 1);
        det.cls = veda::objectClassFromString(typeText);

        if (det.cls == veda::ObjectClass::Unknown) {
            // objectClassFromString이 "Human"/"Vehicle"/"Head"/"LicensePlate" 외의 문자열은
            // 전부 Unknown으로 처리하는데, 이게 파싱 실패가 아니라 "정상적으로 인식된 미지원 값"이라
            // 별도로 로그를 안 남기면 blur가 조용히 걸러지는 원인을 추적할 수 없음
            ++g_unknownTypeCount;
            if (g_unknownTypeCount == 1 || g_unknownTypeCount % 100 == 0) {
                logError(kIface, "ch=" + std::to_string(raw.channelId) + " id=" + std::to_string(det.id) +
                                     " 인식 안 되는 Type=\"" + std::string(typeText) + "\" -> Unknown 처리 (누적 " +
                                     std::to_string(g_unknownTypeCount) + "건)");
            }
        }

        result.objects.push_back(std::move(det));
    }

    return result;
}