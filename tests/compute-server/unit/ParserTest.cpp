/**
 * @file    ParserTest.cpp
 * @brief   OnvifParser 격리 단위 테스트 (보안 패치 회귀 방지)
 *
 * @details
 * 감사에서 확인한 구조적 면역(XXE / Billion Laughs / 무한 루프 / OOB)과
 * 적용된 완화책(W1 로그 정화, W2 NaN/Inf 차단, W3 객체 수 상한)을 고정한다.
 *
 * 의존성: OnvifParser 는 Logger.h(헤더 온리)와 Contract.h 만 참조하므로
 *         소켓/스레드 없이 완전히 격리 테스트가 가능하다.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <string_view>

#include "Contract.h"
#include "parser/OnvifParser.h"

namespace {

/// @brief 파서 상한과 동일한 값 (OnvifParser.cpp 의 익명 네임스페이스 상수와 일치해야 함)
constexpr std::size_t kMaxObjectsPerFrame = 256;

domain::RawPacket makePacket(std::string_view xml, veda::ChannelId ch = 0) {
    domain::RawPacket raw;
    raw.channelId = ch;
    raw.bytes.assign(xml.begin(), xml.end());
    raw.recvTime = std::chrono::system_clock::now();
    return raw;
}

/// @brief 유효한 <tt:Frame> 골격을 만들어 준다 (objectsXml 을 본문에 끼움)
std::string makeFrame(const std::string& objectsXml, const std::string& utc = "2026-07-27T12:00:00.000Z") {
    return "<tt:Frame UtcTime=\"" + utc +
           "\">"
           "<tt:Transformation>"
           "<tt:Translate x=\"-1.0\" y=\"1.0\"/>"
           "<tt:Scale x=\"0.002\" y=\"-0.002\"/>"
           "</tt:Transformation>" +
           objectsXml + "</tt:Frame>";
}

std::string makeObject(int id, const std::string& type, double l = 100, double t = 100, double r = 200,
                       double b = 400) {
    return "<tt:Object ObjectId=\"" + std::to_string(id) +
           "\">"
           "<tt:Appearance><tt:Shape><tt:BoundingBox left=\"" +
           std::to_string(l) + "\" top=\"" + std::to_string(t) + "\" right=\"" + std::to_string(r) + "\" bottom=\"" +
           std::to_string(b) +
           "\"/></tt:Shape>"
           "<tt:Class><tt:Type Likelihood=\"0.9\">" +
           type +
           "</tt:Type></tt:Class></tt:Appearance>"
           "</tt:Object>";
}

}  // namespace

// ============================================================================
// 1. 기본 파싱 동작
// ============================================================================

TEST(ParserTest, ParsesValidFrameIntoObjects) {
    OnvifParser parser;
    const auto raw = makePacket(makeFrame(makeObject(1, "Human") + makeObject(2, "Vehicle")), 3);

    const auto frame = parser.parse(raw);

    EXPECT_EQ(frame.channelId, 3) << "channelId 는 입력 패킷에서 그대로 복사되어야 함";
    EXPECT_GT(frame.utcTime, 0) << "UtcTime 이 epoch ms 로 파싱되어야 함";
    ASSERT_EQ(frame.objects.size(), 2u);
    EXPECT_EQ(frame.objects[0].cls, veda::ObjectClass::Human);
    EXPECT_EQ(frame.objects[1].cls, veda::ObjectClass::Vehicle);
}

TEST(ParserTest, ParsesParentAttributeForBlurObjects) {
    OnvifParser parser;
    const std::string obj =
        "<tt:Object ObjectId=\"7\" Parent=\"5\">"
        "<tt:Appearance><tt:Shape>"
        "<tt:BoundingBox left=\"10\" top=\"10\" right=\"20\" bottom=\"20\"/>"
        "</tt:Shape>"
        "<tt:Class><tt:Type>Head</tt:Type></tt:Class></tt:Appearance></tt:Object>";

    const auto frame = parser.parse(makePacket(makeFrame(obj)));

    ASSERT_EQ(frame.objects.size(), 1u);
    ASSERT_TRUE(frame.objects[0].parentId.has_value());
    EXPECT_EQ(*frame.objects[0].parentId, 5u);
    EXPECT_EQ(frame.objects[0].cls, veda::ObjectClass::Head);
}

// ============================================================================
// 2. 구조적 면역 — XXE / Billion Laughs / 재귀
// ============================================================================

TEST(ParserTest, XxeExternalEntityIsNotResolved) {
    // 파서에는 엔티티 해석 엔진이 없다. DOCTYPE/ENTITY 는 단순 바이트로 취급되어야 하며
    // 파일을 읽거나 네트워크에 접근하는 일이 있어서는 안 된다.
    OnvifParser parser;
    const std::string xxe =
        "<?xml version=\"1.0\"?>"
        "<!DOCTYPE foo [ <!ENTITY xxe SYSTEM \"file:///etc/passwd\"> ]>" +
        makeFrame(makeObject(1, "&xxe;"));

    const auto frame = parser.parse(makePacket(xxe));

    // 엔티티가 확장되지 않았으므로 Type 문자열은 인식 불가 -> Unknown (크래시/유출 없음)
    ASSERT_EQ(frame.objects.size(), 1u);
    EXPECT_EQ(frame.objects[0].cls, veda::ObjectClass::Unknown)
        << "외부 엔티티는 절대 해석되지 않아야 하며, 미지 문자열로 남아야 함";
}

TEST(ParserTest, BillionLaughsDoesNotExpand) {
    // 엔티티 확장 단계가 존재하지 않으므로 지수적 폭발이 성립하지 않는다.
    OnvifParser parser;
    std::string bomb =
        "<!DOCTYPE lolz ["
        "<!ENTITY lol \"lol\">"
        "<!ENTITY lol2 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">"
        "<!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">"
        "<!ENTITY lol4 \"&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;\">"
        "]>";
    bomb += makeFrame(makeObject(1, "&lol4;"));

    const auto frame = parser.parse(makePacket(bomb));

    // 메모리/CPU 폭발 없이 즉시 반환되어야 함
    ASSERT_EQ(frame.objects.size(), 1u);
    EXPECT_EQ(frame.objects[0].cls, veda::ObjectClass::Unknown);
}

TEST(ParserTest, DeeplyNestedXmlDoesNotOverflowStack) {
    // 파서는 재귀를 쓰지 않으므로 중첩 깊이는 스택에 영향을 주지 않는다.
    OnvifParser parser;
    std::string deep;
    for (int i = 0; i < 20000; ++i) deep += "<a>";
    deep += makeFrame(makeObject(1, "Human"));
    for (int i = 0; i < 20000; ++i) deep += "</a>";

    const auto frame = parser.parse(makePacket(deep));  // 크래시하지 않아야 함
    EXPECT_EQ(frame.objects.size(), 1u);
}

// ============================================================================
// 3. 절단·손상 입력 — fail-closed (예외 금지)
// ============================================================================

TEST(ParserTest, MalformedInputsReturnEmptyFrameWithoutThrowing) {
    OnvifParser parser;

    struct Case {
        const char* name;
        std::string xml;
    };
    const std::vector<Case> cases = {
        {"empty payload", ""},
        {"no frame tag", "<tt:Nothing/>"},
        {"unterminated frame", "<tt:Frame UtcTime=\"2026-07-27T12:00:00.000Z\">"},
        {"missing UtcTime", "<tt:Frame></tt:Frame>"},
        {"bad UtcTime format", "<tt:Frame UtcTime=\"not-a-time\"></tt:Frame>"},
        {"missing Transformation", "<tt:Frame UtcTime=\"2026-07-27T12:00:00.000Z\"></tt:Frame>"},
        {"truncated mid-tag", "<tt:Frame UtcTime=\"2026-07-27T12:00"},
        {"unterminated quote", "<tt:Frame UtcTime=\"2026"},
    };

    for (const auto& c : cases) {
        EXPECT_NO_THROW({
            const auto frame = parser.parse(makePacket(c.xml));
            EXPECT_TRUE(frame.objects.empty()) << c.name << " 는 빈 프레임을 반환해야 함";
        }) << c.name << " 에서 예외가 발생하면 안 됨 (파이프라인 스레드 보호)";
    }
}

TEST(ParserTest, EmbeddedNullBytesDoNotTruncateScanning) {
    // payload 는 length 기반 string_view 이므로 \0 이 스캔을 끊지 않아야 한다.
    OnvifParser parser;
    std::string xml = makeFrame(makeObject(1, "Human"));
    xml.insert(0, std::string("\0\0", 2));  // 선두에 널 바이트

    EXPECT_NO_THROW({
        const auto frame = parser.parse(makePacket(xml));
        EXPECT_EQ(frame.objects.size(), 1u) << "널 바이트가 있어도 이후 태그를 계속 찾아야 함";
    });
}

TEST(ParserTest, ObjectMissingIdOrBboxIsSkippedButFrameSurvives) {
    OnvifParser parser;
    const std::string objects =
        "<tt:Object><tt:Class><tt:Type>Human</tt:Type></tt:Class></tt:Object>"  // ObjectId 없음
        + makeObject(2, "Vehicle");                                             // 정상

    const auto frame = parser.parse(makePacket(makeFrame(objects)));

    ASSERT_EQ(frame.objects.size(), 1u) << "잘못된 객체만 스킵되고 정상 객체는 살아남아야 함";
    EXPECT_EQ(frame.objects[0].id, 2u);
}

// ============================================================================
// 4. [W3] DoS 증폭 방어 — 객체 수 상한
// ============================================================================

TEST(ParserTest, W3_ObjectCountIsCappedAtMaxObjectsPerFrame) {
    OnvifParser parser;
    std::string objects;
    const int flood = static_cast<int>(kMaxObjectsPerFrame) + 200;  // 상한을 크게 초과
    for (int i = 0; i < flood; ++i) objects += makeObject(i + 1, "Human");

    const auto frame = parser.parse(makePacket(makeFrame(objects)));

    EXPECT_LE(frame.objects.size(), kMaxObjectsPerFrame)
        << "[W3] 증폭 페이로드가 상한을 넘어 메모리를 부풀리면 안 됨";
    EXPECT_EQ(frame.objects.size(), kMaxObjectsPerFrame) << "상한까지는 정상적으로 채워야 함";
}

TEST(ParserTest, W3_UnderCapAllObjectsAreParsed) {
    OnvifParser parser;
    std::string objects;
    for (int i = 0; i < 10; ++i) objects += makeObject(i + 1, "Human");

    const auto frame = parser.parse(makePacket(makeFrame(objects)));
    EXPECT_EQ(frame.objects.size(), 10u) << "상한 이하에서는 잘리면 안 됨";
}

// ============================================================================
// 5. [W2] NaN / Inf 좌표 차단
// ============================================================================

TEST(ParserTest, W2_NonFiniteCoordinatesAreRejected) {
    OnvifParser parser;
    // from_chars 는 "inf"/"nan" 을 그대로 파싱하므로 파서가 걸러야 한다.
    const std::string objects =
        "<tt:Object ObjectId=\"1\">"
        "<tt:Appearance><tt:Shape>"
        "<tt:BoundingBox left=\"inf\" top=\"10\" right=\"20\" bottom=\"20\"/>"
        "</tt:Shape><tt:Class><tt:Type>Human</tt:Type></tt:Class></tt:Appearance></tt:Object>" +
        makeObject(2, "Vehicle");  // 정상 객체

    const auto frame = parser.parse(makePacket(makeFrame(objects)));

    for (const auto& o : frame.objects) {
        EXPECT_TRUE(std::isfinite(o.box.l)) << "[W2] NaN/Inf 좌표 객체가 통과하면 안 됨";
        EXPECT_TRUE(std::isfinite(o.box.r));
        EXPECT_TRUE(std::isfinite(o.box.t));
        EXPECT_TRUE(std::isfinite(o.box.b));
    }
    EXPECT_EQ(frame.objects.size(), 1u) << "비유한 좌표 객체만 폐기되고 정상 객체는 남아야 함";
}

// ============================================================================
// 6. [W1] 로그 정화 — 신뢰 불가 문자열이 크래시를 유발하지 않음
// ============================================================================

TEST(ParserTest, W1_HostileTypeStringDoesNotCrashOrThrow) {
    // sanitizeForLog 는 내부 정적 함수라 직접 호출할 수 없으므로,
    // 로그 주입을 노린 문자열이 파서를 통과해도 안전한지(예외/크래시 없음)를 확인한다.
    OnvifParser parser;
    const std::string hostile = "evil\r\n2026-01-01,FAKE,injected,row\"quote";
    const auto frame = parser.parse(makePacket(makeFrame(makeObject(1, hostile))));

    EXPECT_EQ(frame.objects.size(), 1u);
    EXPECT_EQ(frame.objects[0].cls, veda::ObjectClass::Unknown);
}

// ============================================================================
// 7. 경계 판정 플래그 (risk 정확도의 핵심)
// ============================================================================

TEST(ParserTest, BottomTruncatedFlagIsSetWhenBoxTouchesBottomEdge) {
    OnvifParser parser;
    // Transformation: x' = (0.002*x - 1 + 1)/2 = 0.001x , y' = (1 - (-0.002*y + 1))/2 = 0.001y
    // bottom=1000 -> b = 1.0 (하단 경계)
    const auto frame = parser.parse(makePacket(makeFrame(makeObject(1, "Human", 100, 100, 200, 1000))));

    ASSERT_EQ(frame.objects.size(), 1u);
    EXPECT_TRUE(frame.objects[0].bottomTruncated) << "아래변이 경계에 닿으면 bottomTruncated 가 서야 함";
    EXPECT_TRUE(frame.objects[0].touchesBorder) << "bottomTruncated 이면 touchesBorder 도 참이어야 함";
}

TEST(ParserTest, InteriorBoxHasNoEdgeFlags) {
    OnvifParser parser;
    const auto frame = parser.parse(makePacket(makeFrame(makeObject(1, "Human", 300, 300, 500, 500))));

    ASSERT_EQ(frame.objects.size(), 1u);
    EXPECT_FALSE(frame.objects[0].bottomTruncated);
    EXPECT_FALSE(frame.objects[0].touchesBorder);
}
