/**
 * @file    SanitizerTest.cpp
 * @brief   ContainmentSanitizer 격리 단위 테스트 (보안 패치 회귀 방지)
 *
 * @details
 * 감사에서 확인한 항목을 고정한다:
 *  - [W2] 임계값 범위 검증 (음수/1 초과 -> std::invalid_argument)
 *  - [W1] 객체 수 상한 256 (파서와 정렬) 및 fail-open 정책
 *  - 무할당 설계 (스택 bitset + in-place 압축)
 *  - 기하학적 예외 (역전/0면적/음수 좌표)에서 0 나눗셈·크래시 없음
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "Contract.h"
#include "sanitize/ContainmentSanitizer.h"

namespace {

/// @brief Sanitizer 상한과 동일한 값 (ContainmentSanitizer.cpp 의 상수와 반드시 일치)
constexpr std::size_t kMaxObjectsPerFrame = 256;

/// @name 힙 할당 계측 (무할당 검증용)
/// @{
long g_allocCount = 0;
bool g_allocCounting = false;
/// @}

domain::DetectedObject makeObject(veda::ObjectId id, veda::ObjectClass cls, double l, double t, double r, double b) {
    domain::DetectedObject o;
    o.id = id;
    o.cls = cls;
    o.box.l = l;
    o.box.t = t;
    o.box.r = r;
    o.box.b = b;
    return o;
}

domain::ChannelFrame makeFrame(std::vector<domain::DetectedObject> objects) {
    domain::ChannelFrame f;
    f.channelId = 0;
    f.utcTime = 1;
    f.objects = std::move(objects);
    return f;
}

bool containsId(const domain::ChannelFrame& f, veda::ObjectId id) {
    for (const auto& o : f.objects) {
        if (o.id == id) return true;
    }
    return false;
}

}  // namespace

void* operator new(std::size_t n) {
    if (g_allocCounting) ++g_allocCount;
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ============================================================================
// 1. [W2] 임계값 검증 — 조용한 대량 삭제 방지 (위험 방향 실패 차단)
// ============================================================================

TEST(SanitizerTest, W2_ValidThresholdsAreAccepted) {
    EXPECT_NO_THROW({ ContainmentSanitizer s(0.5, 0.9); });
    EXPECT_NO_THROW({ ContainmentSanitizer s(0.0, 0.0); }) << "경계값 0.0 은 유효";
    EXPECT_NO_THROW({ ContainmentSanitizer s(1.0, 1.0); }) << "경계값 1.0 은 유효";
}

TEST(SanitizerTest, W2_NegativeIouThresholdThrows) {
    // 음수 임계값이면 "0.0 > -0.1" 이 참이 되어 겹치지 않은 위험 객체까지 전부 삭제된다.
    // -> 경보 미발생(과소 검출). 조립 시점에 반드시 실패해야 한다.
    EXPECT_THROW({ ContainmentSanitizer s(-0.1, 0.9); }, std::invalid_argument);
}

TEST(SanitizerTest, W2_NegativeContainThresholdThrows) {
    EXPECT_THROW({ ContainmentSanitizer s(0.5, -0.001); }, std::invalid_argument);
}

TEST(SanitizerTest, W2_ThresholdAboveOneThrows) {
    EXPECT_THROW({ ContainmentSanitizer s(1.5, 0.9); }, std::invalid_argument);
    EXPECT_THROW({ ContainmentSanitizer s(0.5, 2.0); }, std::invalid_argument);
}

TEST(SanitizerTest, W2_ThrownMessageIdentifiesOffendingKey) {
    try {
        ContainmentSanitizer s(-0.1, 0.9);
        FAIL() << "음수 임계값은 반드시 던져야 함";
    } catch (const std::invalid_argument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("sanitizerIouThresh"), std::string::npos)
            << "운영자가 즉시 교정할 수 있도록 설정 키 이름이 포함되어야 함";
    }
}

// ============================================================================
// 2. 팬텀 제거 규칙 A / B
// ============================================================================

TEST(SanitizerTest, RuleA_RiskObjectOverlappingBlurObjectIsRemoved) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    // Human(1) 이 Head(2) 와 거의 완전히 겹침 -> Human 이 팬텀으로 제거되어야 함
    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Human, 0.10, 0.10, 0.20, 0.20),
        makeObject(2, veda::ObjectClass::Head, 0.10, 0.10, 0.20, 0.20),
    });

    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_FALSE(containsId(out, 1)) << "규칙 A: 부위 객체와 겹친 RISK 후보는 제거";
    EXPECT_TRUE(containsId(out, 2)) << "부위 객체 자체는 유지";
}

TEST(SanitizerTest, RuleB_SmallerObjectContainedInSameClassIsRemoved) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    // 작은 Human(2) 이 큰 Human(1) 안에 완전히 포함 -> 작은 쪽 제거
    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Human, 0.0, 0.0, 1.0, 1.0),
        makeObject(2, veda::ObjectClass::Human, 0.4, 0.4, 0.5, 0.5),
    });

    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_TRUE(containsId(out, 1)) << "큰 쪽은 유지";
    EXPECT_FALSE(containsId(out, 2)) << "규칙 B: 같은 클래스에 포함된 작은 쪽은 제거";
}

TEST(SanitizerTest, RuleB_DoesNotApplyAcrossDifferentClasses) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    // Human 이 Vehicle 안에 있어도 서로 다른 실제 객체일 수 있으므로 제거하면 안 됨
    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 1.0, 1.0),
        makeObject(2, veda::ObjectClass::Human, 0.4, 0.4, 0.5, 0.5),
    });

    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_TRUE(containsId(out, 1));
    EXPECT_TRUE(containsId(out, 2)) << "클래스가 다르면 포함 관계라도 유지되어야 함";
}

TEST(SanitizerTest, NonOverlappingObjectsAreAllKept) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Human, 0.0, 0.0, 0.1, 0.1),
        makeObject(2, veda::ObjectClass::Human, 0.8, 0.8, 0.9, 0.9),
        makeObject(3, veda::ObjectClass::Vehicle, 0.4, 0.4, 0.5, 0.5),
    });

    const auto out = sanitizer.sanitize(std::move(frame));
    EXPECT_EQ(out.objects.size(), 3u) << "겹치지 않으면 아무것도 제거되면 안 됨";
}

// ============================================================================
// 3. [W1] 상한 정렬 및 fail-open 정책
// ============================================================================

TEST(SanitizerTest, W1_CapMatchesParserCap) {
    // 파서 상한(256)과 어긋나면 129~256 구간이 'sanitize 가 생략되는 사각지대'가 된다.
    // 이 테스트는 상수 정렬이 깨지는 회귀를 잡기 위한 것.
    ContainmentSanitizer sanitizer(0.5, 0.9);

    std::vector<domain::DetectedObject> objs;
    for (std::size_t i = 0; i < kMaxObjectsPerFrame; ++i) {
        // 전부 동일 위치의 Human -> 규칙 B 로 다수가 제거되어야 정상 동작
        objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), veda::ObjectClass::Human, 0.1, 0.1, 0.2, 0.2));
    }
    // 하나만 크게 만들어 나머지가 포함되도록 함
    objs[0] = makeObject(1, veda::ObjectClass::Human, 0.0, 0.0, 1.0, 1.0);

    auto frame = makeFrame(objs);
    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_LT(out.objects.size(), kMaxObjectsPerFrame)
        << "[W1] 상한(256) 이내에서는 sanitize 가 반드시 수행되어야 함 (사각지대 없음)";
}

TEST(SanitizerTest, W1_FailOpenAboveCapKeepsAllObjects) {
    // 상한 초과 시에는 스킵(fail-open) — 위험 객체를 지우느니 팬텀을 남기는 쪽이 안전하다.
    ContainmentSanitizer sanitizer(0.5, 0.9);

    std::vector<domain::DetectedObject> objs;
    const std::size_t over = kMaxObjectsPerFrame + 1;
    for (std::size_t i = 0; i < over; ++i) {
        objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), veda::ObjectClass::Human, 0.1, 0.1, 0.2, 0.2));
    }

    auto frame = makeFrame(objs);
    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_EQ(out.objects.size(), over) << "[W1] 상한 초과 시 fail-open — 원본이 그대로 통과해야 함";
}

// ============================================================================
// 4. 무할당 설계 (스택 bitset + in-place 압축)
// ============================================================================

TEST(SanitizerTest, ZeroAllocationDuringSanitizeSteadyState) {
    ContainmentSanitizer sanitizer(0.5, 0.9);

    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 64; ++i) {
        objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), veda::ObjectClass::Human,
                                  0.01 * i, 0.01 * i, 0.01 * i + 0.005, 0.01 * i + 0.005));
    }
    auto frame = makeFrame(objs);

    // 프레임 이동만 하고 sanitize 내부에서 새 할당이 없어야 한다
    // (drop 마스크는 스택 std::bitset, 압축은 in-place resize)
    g_allocCount = 0;
    g_allocCounting = true;
    auto out = sanitizer.sanitize(std::move(frame));
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "sanitize 는 스택 bitset + in-place 압축이므로 힙 할당이 0이어야 함";
    EXPECT_FALSE(out.objects.empty());
}

TEST(SanitizerTest, InPlaceCompactionPreservesCapacity) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Human, 0.0, 0.0, 1.0, 1.0),
        makeObject(2, veda::ObjectClass::Human, 0.4, 0.4, 0.5, 0.5),  // 제거될 객체
    });
    const std::size_t capacityBefore = frame.objects.capacity();

    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_EQ(out.objects.capacity(), capacityBefore) << "축소 resize 는 재할당하지 않고 capacity 를 유지해야 함";
    EXPECT_LT(out.objects.size(), capacityBefore);
}

// ============================================================================
// 5. 기하학적 예외 — 0 나눗셈 / 크래시 없음
// ============================================================================

TEST(SanitizerTest, DegenerateBoxesDoNotCrashOrRemoveValidObjects) {
    ContainmentSanitizer sanitizer(0.5, 0.9);

    auto frame = makeFrame({
        makeObject(1, veda::ObjectClass::Human, 0.5, 0.5, 0.5, 0.5),    // 0 면적
        makeObject(2, veda::ObjectClass::Human, 0.9, 0.9, 0.1, 0.1),    // 역전 (l>r, t>b)
        makeObject(3, veda::ObjectClass::Vehicle, -5.0, -5.0, -4.0, -4.0),  // 음수 좌표
        makeObject(4, veda::ObjectClass::Human, 0.2, 0.2, 0.3, 0.3),    // 정상
    });

    EXPECT_NO_THROW({
        const auto out = sanitizer.sanitize(std::move(frame));
        // 0 면적/역전 박스는 iou/ioMin 이 0 을 반환하므로 어떤 규칙에도 걸리지 않고 통과해야 함
        EXPECT_EQ(out.objects.size(), 4u) << "퇴화 박스는 제거 대상이 아니며 크래시도 없어야 함";
    });
}

TEST(SanitizerTest, EmptyFrameIsHandled) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    const auto out = sanitizer.sanitize(makeFrame({}));
    EXPECT_TRUE(out.objects.empty());
}

TEST(SanitizerTest, SingleObjectFrameIsUnchanged) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    const auto out = sanitizer.sanitize(makeFrame({makeObject(1, veda::ObjectClass::Human, 0.1, 0.1, 0.2, 0.2)}));
    ASSERT_EQ(out.objects.size(), 1u);
    EXPECT_EQ(out.objects[0].id, 1u);
}

TEST(SanitizerTest, MetadataFieldsArePreserved) {
    ContainmentSanitizer sanitizer(0.5, 0.9);
    domain::ChannelFrame frame = makeFrame({makeObject(1, veda::ObjectClass::Human, 0.1, 0.1, 0.2, 0.2)});
    frame.channelId = 2;
    frame.utcTime = 1700000000000LL;

    const auto out = sanitizer.sanitize(std::move(frame));

    EXPECT_EQ(out.channelId, 2);
    EXPECT_EQ(out.utcTime, 1700000000000LL);
}
