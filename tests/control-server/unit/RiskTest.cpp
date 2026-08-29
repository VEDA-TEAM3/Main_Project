/**
 * @file    RiskTest.cpp
 * @brief   ThresholdRiskPolicy 격리 단위 테스트 (조용한 무경보 경로 회귀 방지)
 *
 * @details
 * 감사에서 확인한 항목을 고정한다. 이 계층의 결함은 전부 '경보가 조용히 안 나가는'
 * 방향으로 실패하므로, 아래 테스트가 깨지면 곧 인명 위험이다.
 *  - [2.1] 비유한 좌표를 O(N) 사전 단계에서 제외 (NaN 이 차량 판정을 통째로 삼키던 경로)
 *  - [2.2] 비유한/음수/역전 임계값을 생성자에서 거부
 *  - [3.1] gid==0 은 유효한 값 -- 인덱스 센티널(kNoIndex)로 대체 (재현된 경보 소실)
 *  - [3.2] zoneId 범위 검사 (인덱싱 전)
 *  - [4.1] out-parameter 무할당
 *  - 위험 판정 5원칙과 임계값 경계 (<= 이므로 경계값 포함)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

#include "Logger.h"
#include "metric/EuclideanMetric.h"
#include "risk/ThresholdRiskPolicy.h"

namespace {

long g_allocCount = 0;
bool g_allocCounting = false;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr double kWarning = 5.0;
constexpr double kDanger = 2.0;
constexpr int kChannels = 4;

RiskConfig makeConfig(double warning = kWarning, double danger = kDanger) {
    RiskConfig c;
    c.warningDistance = warning;
    c.dangerousDistance = danger;
    return c;
}

std::shared_ptr<IDistanceMetric> makeMetric() { return std::make_shared<EuclideanMetric>(); }

domain::WorldObject makeObject(veda::GlobalId gid, veda::ObjectClass cls, double x, double y,
                               veda::ChannelId zoneId = 0) {
    domain::WorldObject o;
    o.gid = gid;
    o.cls = cls;
    o.pos.x = x;
    o.pos.y = y;
    o.zoneId = zoneId;
    o.sourceChannels.add(zoneId >= 0 ? zoneId : 0);
    return o;
}

domain::WorldFrame makeFrame(std::vector<domain::WorldObject> objects) {
    domain::WorldFrame f;
    f.timestamp = 1000;
    f.objects = std::move(objects);
    return f;
}

const domain::WorldObject* findByGid(const domain::WorldFrame& f, veda::GlobalId gid) {
    for (const auto& o : f.objects) {
        if (o.gid == gid)
            return &o;
    }
    return nullptr;
}

struct LoggerOff {
    LoggerOff() {
        LogConfig c;
        c.level = LogLevel::Off;
        c.console = false;
        c.file = false;
        initLogger(c);
    }
};
const LoggerOff g_loggerOff;

}  // namespace

void* operator new(std::size_t n) {
    if (g_allocCounting)
        ++g_allocCount;
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr)
        throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_allocCounting)
        ++g_allocCount;
    return std::malloc(n != 0 ? n : 1);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ============================================================================
// 1. 생성자 검증 (설정만으로 경보가 죽는 경로 차단)
// ============================================================================

TEST(RiskTest, RejectsNullMetric) {
    EXPECT_THROW(ThresholdRiskPolicy(nullptr, makeConfig(), kChannels), std::invalid_argument);
}

TEST(RiskTest, RejectsChannelCountOutOfRange) {
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(), 0), std::invalid_argument);
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(), -1), std::invalid_argument);
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(), 257), std::invalid_argument);
    EXPECT_NO_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(), 256));
}

TEST(RiskTest, RejectsNonFiniteThresholds) {
    // minDist <= NaN 은 항상 false -> Danger 도 Warning 도 성립하지 않는다 = 경보 완전 사망.
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(kNaN, kDanger), kChannels), std::invalid_argument);
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(kWarning, kNaN), kChannels), std::invalid_argument);
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(kInf, kDanger), kChannels), std::invalid_argument);
}

TEST(RiskTest, RejectsNegativeThresholds) {
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(-1.0, -2.0), kChannels), std::invalid_argument);
}

TEST(RiskTest, RejectsInvertedThresholds) {
    // dangerous > warning 이면 minDist <= dangerous 가 먼저 걸려 Warning 이 영영 도달 불가능해진다.
    // 운영자가 의도한 2단계 판정이 조용히 1단계가 되는 것을 막는다.
    EXPECT_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(5.0, 10.0), kChannels), std::invalid_argument);
}

TEST(RiskTest, AcceptsEqualThresholds) {
    // 같은 값은 역전이 아니다 -- 운영자가 명시적으로 선택한 1단계 판정으로 허용한다.
    EXPECT_NO_THROW(ThresholdRiskPolicy(makeMetric(), makeConfig(3.0, 3.0), kChannels));
}

// ============================================================================
// 2. 임계값 경계 (Warning vs Danger)
// ============================================================================

TEST(RiskTest, DistanceInsideDangerousYieldsDanger) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger);
    EXPECT_DOUBLE_EQ(findByGid(frame, 1)->nearestDist, 1.0);
}

TEST(RiskTest, DistanceBetweenThresholdsYieldsWarning) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, 3.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Warning);
}

TEST(RiskTest, DistanceBeyondWarningYieldsNone) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, 20.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::None);
}

TEST(RiskTest, ExactlyAtDangerousBoundaryIsDanger) {
    // <= 이므로 경계값은 '더 위험한 쪽'으로 분류된다 (안전 방향, 의도적).
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, kDanger, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger) << "경계값은 Danger 에 포함되어야 한다";
}

TEST(RiskTest, ExactlyAtWarningBoundaryIsWarning) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, kWarning, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Warning) << "경계값은 Warning 에 포함되어야 한다";
}

// ============================================================================
// 3. 위험 판정 5원칙
// ============================================================================

TEST(RiskTest, NoVehicleMeansNoRisk) {
    // [원칙 1] 사람만 있으면 아무리 붙어 있어도 전부 None 이다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Human, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, 0.1, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(findByGid(frame, 2)->riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(frame.level, veda::RiskLevel::None);
}

TEST(RiskTest, RiskPropagatesToNearestObjectRaiseOnly) {
    // [원칙 3] 최근접 객체에도 레벨을 부여하되 '올리기만' 한다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 2)->riskLevel, veda::RiskLevel::Danger) << "최근접 사람에게 전파되어야 한다";
}

TEST(RiskTest, PropagationNeverLowersExistingLevel) {
    // Danger 로 이미 표시된 객체를 나중 순회의 Warning 이 깎아내리면 안 된다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    // 사람(gid 3)이 차량1(1m, Danger)과 차량2(4m, Warning) 양쪽의 최근접이 되도록 배치
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Vehicle, 100.0, 0.0),
         makeObject(3, veda::ObjectClass::Human, 1.0, 0.0), makeObject(4, veda::ObjectClass::Human, 96.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 3)->riskLevel, veda::RiskLevel::Danger) << "더 높은 레벨이 유지되어야 한다";
}

TEST(RiskTest, FrameLevelIsMaxOfZoneLevels) {
    // [원칙 4] UI(frame.level) 와 HW(zoneLevels) 가 같은 소스에서 파생된다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0),
                            makeObject(2, veda::ObjectClass::Human, 3.0, 0.0, 0),
                            makeObject(3, veda::ObjectClass::Vehicle, 50.0, 0.0, 1),
                            makeObject(4, veda::ObjectClass::Human, 51.0, 0.0, 1)});
    policy.evaluate(frame, out);

    veda::RiskLevel expected = veda::RiskLevel::None;
    for (const auto& z : out.zoneLevels) {
        if (z.level > expected)
            expected = z.level;
    }
    EXPECT_EQ(frame.level, expected);
    EXPECT_EQ(frame.level, veda::RiskLevel::Danger) << "zone 1 이 Danger 이므로 프레임 전체도 Danger";
}

TEST(RiskTest, ZoneLevelExcludesPropagatedHumanLevel) {
    // [원칙 5] 채널 위험도는 '차량' 판정으로만 결정된다.
    // 사람이 다른 zone 에 있어도 그 zone 이 올라가면 안 된다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, /*zone=*/0),
                            makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, /*zone=*/2)});
    policy.evaluate(frame, out);

    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::Danger) << "차량이 있는 zone 0 만 올라야 한다";
    EXPECT_EQ(out.zoneLevels[2].level, veda::RiskLevel::None)
        << "사람에게 전파된 레벨이 zone 집계에 섞이면 UI/HW 가 어긋난다";
    EXPECT_EQ(findByGid(frame, 2)->riskLevel, veda::RiskLevel::Danger) << "객체 마커 자체는 전파된다";
}

TEST(RiskTest, VehicleToVehicleDistanceCounts) {
    // [원칙 2] 차량↔차량도 판정 대상이다 (사람↔사람만 제외).
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(2, veda::ObjectClass::Vehicle, 1.5, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger);
    EXPECT_EQ(findByGid(frame, 2)->riskLevel, veda::RiskLevel::Danger);
}

TEST(RiskTest, LoneVehicleHasNoRisk) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(findByGid(frame, 1)->nearestObj, 0u);
    EXPECT_EQ(frame.level, veda::RiskLevel::None);
}

// ============================================================================
// 4. gid == 0 센티널 버그 (재현된 경보 소실)
// ============================================================================

TEST(RiskTest, NearestObjectWithGidZeroIsStillJudged) {
    // [ 재현된 결함 ] 이전 구현은 nearestGid = 0 을 "못 찾음" 센티널로 썼다.
    // ConcatFuser 가 wObj.gid = 0 을 초기값으로 넣으므로 gid==0 인 객체는 실제로 존재하고,
    // 그 객체가 최근접이면 차량이 판정에서 빠져 경보가 사라졌다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(5, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(0, veda::ObjectClass::Human, 1.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 5)->riskLevel, veda::RiskLevel::Danger)
        << "gid==0 객체가 최근접이어도 경보가 나가야 한다 (센티널 충돌 회귀)";
    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::Danger);
}

TEST(RiskTest, GidZeroObjectReceivesPropagatedLevel) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(5, veda::ObjectClass::Vehicle, 0.0, 0.0), makeObject(0, veda::ObjectClass::Human, 1.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 0)->riskLevel, veda::RiskLevel::Danger);
}

TEST(RiskTest, TwoObjectsSharingGidZeroDoNotConfuseNearestLookup) {
    // 인덱스로 최근접을 식별하므로 gid 가 겹쳐도 올바른 객체 하나에만 전파된다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(7, veda::ObjectClass::Vehicle, 0.0, 0.0),
                            makeObject(0, veda::ObjectClass::Human, 1.0, 0.0),
                            makeObject(0, veda::ObjectClass::Human, 50.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(frame.objects[0].riskLevel, veda::RiskLevel::Danger);
    EXPECT_EQ(frame.objects[1].riskLevel, veda::RiskLevel::Danger) << "가까운 쪽에 전파";
    EXPECT_EQ(frame.objects[2].riskLevel, veda::RiskLevel::None) << "먼 쪽은 건드리지 않아야 한다";
}

// ============================================================================
// 5. NaN / Inf 좌표
//
// @warning [ 이 절의 테스트는 '변이 검증(mutation test)을 통과하지 못한다' ]
// 1단계의 유한성 필터를 제거해도 아래 테스트는 전부 통과한다. 이유는 NaN 의 비교 의미
// 자체에 있다 -- minDist 는 max() 로 시작하고 `dist < minDist` 는 NaN 에 대해 false 이므로,
// 필터가 없어도 NaN 객체는 최근접으로 '뽑히지 않는다'. 즉 이 계층에서 NaN 의 실패 방향은
// 이미 안전 쪽(무시)이며, 필터는 그것을 '명시적이고 로그로 남는' 동작으로 바꾸는
// 다층 방어(defense-in-depth)다.
//
// NaN 이 실제로 해로운 곳은 융합 계층이다: `dist > dedupMergeDistance_` 는 NaN 에 대해
// false 라 continue 하지 않고 **병합으로 진행**한다. 그쪽은 별도 테스트가 필요하다.
//
// 따라서 아래 테스트들은 회귀 가드가 아니라 **계약 문서**로 읽어야 한다.
// (회귀 가드로 검증된 것은 4절 gid==0 과 7절 무할당이다 -- 둘 다 변이 시 실패 확인됨)
// ============================================================================

TEST(RiskTest, NaNObjectDoesNotSuppressOtherJudgements) {
    // NaN 객체가 섞여도 나머지 차량 판정이 정상이어야 한다.
    // (필터 유무와 무관하게 성립하지만, 판정 결과 자체를 고정해 둘 가치는 있다)
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0),
                            makeObject(2, veda::ObjectClass::Human, 1.0, 0.0),
                            makeObject(3, veda::ObjectClass::Human, kNaN, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger)
        << "NaN 객체가 섞여도 나머지 판정이 살아 있어야 한다";
    EXPECT_EQ(findByGid(frame, 1)->nearestObj, 2u);
}

TEST(RiskTest, NaNVehicleIsExcludedWithoutCrashing) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, kNaN, kNaN), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0)});
    EXPECT_NO_THROW(policy.evaluate(frame, out));

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::None) << "위치를 모르는 차량은 판정하지 않는다";
    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::None);
}

TEST(RiskTest, InfiniteCoordinateIsExcluded) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0),
                            makeObject(2, veda::ObjectClass::Human, kInf, 0.0),
                            makeObject(3, veda::ObjectClass::Human, 1.0, 0.0)});
    policy.evaluate(frame, out);

    EXPECT_EQ(findByGid(frame, 1)->nearestObj, 3u) << "Inf 객체는 후보에서 빠져야 한다";
    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger);
}

TEST(RiskTest, NaNOnlyFrameProducesNoRisk) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, kNaN, 0.0), makeObject(2, veda::ObjectClass::Human, 0.0, kNaN)});
    EXPECT_NO_THROW(policy.evaluate(frame, out));
    EXPECT_EQ(frame.level, veda::RiskLevel::None);
}

// ============================================================================
// 6. zoneId 경계 검사
// ============================================================================

TEST(RiskTest, VehicleWithUnassignedZoneIsExcludedFromZoneAggregation) {
    // SpatialZoneMapper 는 어떤 AABB 에도 속하지 않는 객체에 zoneId = -1 을 준다.
    // 하한 검사가 빠지면 size_t 캐스팅으로 거대 인덱스가 되어 OOB 가 된다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, /*zone=*/-1),
                            makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, /*zone=*/-1)});
    EXPECT_NO_THROW(policy.evaluate(frame, out));

    EXPECT_EQ(findByGid(frame, 1)->riskLevel, veda::RiskLevel::Danger) << "객체 판정 자체는 이뤄진다";
    for (const auto& z : out.zoneLevels) {
        EXPECT_EQ(z.level, veda::RiskLevel::None) << "미배정 차량은 zone 집계에서 빠져야 한다";
    }
}

TEST(RiskTest, VehicleWithZoneIdAboveChannelCountIsExcluded) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, /*zone=*/kChannels),
                            makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, /*zone=*/kChannels)});
    EXPECT_NO_THROW(policy.evaluate(frame, out));

    for (const auto& z : out.zoneLevels) {
        EXPECT_EQ(z.level, veda::RiskLevel::None);
    }
}

TEST(RiskTest, ZoneLevelsAreSizedAndIdentifiedByChannel) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({});
    policy.evaluate(frame, out);

    ASSERT_EQ(out.zoneLevels.size(), static_cast<std::size_t>(kChannels));
    for (int ch = 0; ch < kChannels; ++ch) {
        EXPECT_EQ(out.zoneLevels[static_cast<std::size_t>(ch)].zoneId, ch);
        EXPECT_EQ(out.zoneLevels[static_cast<std::size_t>(ch)].level, veda::RiskLevel::None);
        EXPECT_DOUBLE_EQ(out.zoneLevels[static_cast<std::size_t>(ch)].minDist, -1.0);
    }
}

// ============================================================================
// 7. out-parameter 무할당 및 버퍼 재사용
// ============================================================================

TEST(RiskTest, EvaluateDoesNotAllocateInSteadyState) {
    // [4.1] 값 반환이던 시절에는 zoneLevels 가 매 프레임 새로 할당됐다(초당 10회).
    // out-parameter 로 바꾸면서 resize 가 no-op 이 되어 0회가 되었다.
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, 0),
         makeObject(3, veda::ObjectClass::Vehicle, 10.0, 0.0, 1), makeObject(4, veda::ObjectClass::Human, 12.0, 0.0, 1),
         makeObject(5, veda::ObjectClass::Human, 30.0, 0.0, 2)});

    for (int i = 0; i < 50; ++i)
        policy.evaluate(frame, out);  // warmup

    g_allocCount = 0;
    g_allocCounting = true;
    for (int i = 0; i < 500; ++i) {
        policy.evaluate(frame, out);
    }
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "프레임 경로에서 할당이 발생했다 "
                                  "(out 을 값 반환으로 되돌렸거나 스크래치 버퍼가 지역 변수가 된 경우)";
}

TEST(RiskTest, ReusedOutBufferIsFullyResetBetweenFrames) {
    // 재사용 버퍼는 이전 프레임의 판정을 흘리면 안 된다 (유령 경보 방지).
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;

    auto dangerous = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, 0)});
    policy.evaluate(dangerous, out);
    ASSERT_EQ(out.zoneLevels[0].level, veda::RiskLevel::Danger);

    auto calm = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0),
                           makeObject(2, veda::ObjectClass::Human, 100.0, 0.0, 0)});
    policy.evaluate(calm, out);

    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::None) << "이전 프레임의 Danger 가 남아 있으면 안 된다";
    EXPECT_DOUBLE_EQ(out.zoneLevels[0].minDist, -1.0);
    EXPECT_EQ(calm.level, veda::RiskLevel::None);
}

TEST(RiskTest, ObjectFieldsAreResetEveryFrame) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;

    auto frame = makeFrame(
        {makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0), makeObject(2, veda::ObjectClass::Human, 1.0, 0.0, 0)});
    policy.evaluate(frame, out);
    ASSERT_EQ(frame.objects[0].riskLevel, veda::RiskLevel::Danger);

    // 같은 프레임 객체를 멀리 옮기고 다시 평가
    frame.objects[1].pos.x = 100.0;
    policy.evaluate(frame, out);

    EXPECT_EQ(frame.objects[0].riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(frame.objects[1].riskLevel, veda::RiskLevel::None) << "전파된 레벨도 리셋되어야 한다";
}

TEST(RiskTest, EmptyFrameIsHandledWithoutRisk) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto frame = makeFrame({});
    EXPECT_NO_THROW(policy.evaluate(frame, out));

    EXPECT_EQ(frame.level, veda::RiskLevel::None);
    EXPECT_EQ(out.timestamp, frame.timestamp);
}

TEST(RiskTest, CoastedObjectIsKeptForDisplayButExcludedFromRisk) {
    ThresholdRiskPolicy policy(makeMetric(), makeConfig(), kChannels);
    domain::RiskEvaluation out;
    auto coasted = makeObject(2, veda::ObjectClass::Vehicle, 0.1, 0.0, 0);
    coasted.sourceChannels = {};
    auto frame = makeFrame({makeObject(1, veda::ObjectClass::Vehicle, 0.0, 0.0, 0), coasted});

    policy.evaluate(frame, out);

    ASSERT_EQ(frame.objects.size(), 2U);
    EXPECT_EQ(frame.objects[0].riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(frame.objects[1].riskLevel, veda::RiskLevel::None);
    EXPECT_EQ(out.zoneLevels[0].level, veda::RiskLevel::None);
    EXPECT_EQ(frame.level, veda::RiskLevel::None);
}

TEST(RiskFrameContractTest, PreservesServerResolvedZoneId) {
    veda::RiskFrame frame;
    veda::RiskObject object;
    object.gid = 7;
    object.zoneId = 3;
    frame.objects.push_back(object);

    std::string payload;
    veda::encodeInto(frame, payload);
    const auto decoded = nlohmann::json::parse(payload).get<veda::RiskFrame>();

    ASSERT_EQ(decoded.objects.size(), 1U);
    EXPECT_EQ(decoded.objects.front().zoneId, 3);
}
