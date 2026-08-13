/**
 * @file    RouterTest.cpp
 * @brief   ParentBasedRouter 격리 단위 테스트 (보안 패치 회귀 방지)
 *
 * @details
 * 감사에서 확인한 항목을 고정한다:
 *  - [W1] 출력 파라미터 방식의 무할당 (clear() 재사용, 이전 프레임 잔여물 없음)
 *  - [W2] 벤더 별칭 정규화 ("Car" -> Vehicle 등)
 *  - 2-신호 판정 로직 (parentId OR cls)
 *  - 개인정보 격리 경계 (Head/LicensePlate 가 risk 로 새지 않음)
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "Contract.h"
#include "route/ParentBasedRouter.h"

namespace {

long g_allocCount = 0;
bool g_allocCounting = false;

domain::DetectedObject makeObject(veda::ObjectId id, veda::ObjectClass cls, bool withParent = false) {
    domain::DetectedObject o;
    o.id = id;
    o.cls = cls;
    if (withParent) o.parentId = static_cast<veda::ObjectId>(id + 1000);
    o.box.l = 0.1;
    o.box.t = 0.1;
    o.box.r = 0.2;
    o.box.b = 0.2;
    return o;
}

domain::ChannelFrame makeFrame(std::vector<domain::DetectedObject> objects) {
    domain::ChannelFrame f;
    f.channelId = 0;
    f.utcTime = 1;
    f.objects = std::move(objects);
    return f;
}

bool contains(const std::vector<domain::DetectedObject>& v, veda::ObjectId id) {
    for (const auto& o : v) {
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
// 1. 2-신호 판정 로직
// ============================================================================

TEST(RouterTest, BlurClassGoesToBlurStream) {
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Head), makeObject(2, veda::ObjectClass::LicensePlate)}),
                 out);

    EXPECT_EQ(out.blur.size(), 2u);
    EXPECT_TRUE(out.risk.empty());
}

TEST(RouterTest, RiskClassWithoutParentGoesToRiskStream) {
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Human), makeObject(2, veda::ObjectClass::Vehicle)}), out);

    EXPECT_EQ(out.risk.size(), 2u);
    EXPECT_TRUE(out.blur.empty());
}

TEST(RouterTest, ParentIdAloneRoutesToBlurEvenWhenClassIsUnknown) {
    // 신호 이중화: Type 문자열 파싱이 실패해도 parentId 가 blur 대상을 구제해야 한다.
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Unknown, /*withParent=*/true)}), out);

    ASSERT_EQ(out.blur.size(), 1u) << "parentId 만으로도 blur 로 구제되어야 함 (개인정보 노출 방지)";
    EXPECT_TRUE(out.risk.empty());
}

TEST(RouterTest, BlurClassAloneRoutesToBlurEvenWithoutParentId) {
    // 반대 방향의 구제: Parent 속성 파싱이 실패해도 cls 가 구제해야 한다.
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Head, /*withParent=*/false)}), out);

    ASSERT_EQ(out.blur.size(), 1u) << "cls 만으로도 blur 로 구제되어야 함";
}

TEST(RouterTest, BothSignalsFailingResultsInDropNotRisk) {
    // 두 신호가 동시에 실패하면 drop 이어야 하며, 절대 risk 로 가면 안 된다.
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Unknown, /*withParent=*/false)}), out);

    EXPECT_TRUE(out.blur.empty());
    EXPECT_TRUE(out.risk.empty()) << "미분류 객체는 drop 되어야지 risk 로 가면 안 됨";
}

TEST(RouterTest, ParentIdTakesPrecedenceOverRiskClass) {
    // [I1] parentId 가 cls 보다 먼저 평가되므로, Parent 를 가진 Vehicle 은 blur 로 흡수된다.
    // 이는 의도된 동작이며(부위 객체로 간주), 회귀 감지를 위해 명시적으로 고정한다.
    ParentBasedRouter router;
    RouteResult out;
    router.route(makeFrame({makeObject(1, veda::ObjectClass::Vehicle, /*withParent=*/true)}), out);

    EXPECT_EQ(out.blur.size(), 1u) << "Parent 가 있으면 risk 클래스라도 blur 로 라우팅됨 (의도된 우선순위)";
    EXPECT_TRUE(out.risk.empty());
}

// ============================================================================
// 2. 개인정보 격리 경계 — Head/LicensePlate 가 risk 로 새지 않음
// ============================================================================

TEST(RouterTest, PrivacyBoundary_BlurClassesNeverReachRiskStream) {
    ParentBasedRouter router;
    RouteResult out;

    // parentId 유무의 모든 조합 × 두 blur 클래스
    router.route(makeFrame({
                     makeObject(1, veda::ObjectClass::Head, true),
                     makeObject(2, veda::ObjectClass::Head, false),
                     makeObject(3, veda::ObjectClass::LicensePlate, true),
                     makeObject(4, veda::ObjectClass::LicensePlate, false),
                 }),
                 out);

    for (const auto& o : out.risk) {
        EXPECT_FALSE(veda::isBlurClass(o.cls))
            << "개인정보 클래스가 risk 스트림(=다른 MQTT 토픽)으로 유출되면 안 됨";
    }
    EXPECT_EQ(out.blur.size(), 4u);
    EXPECT_TRUE(out.risk.empty());
}

TEST(RouterTest, PrivacyBoundary_RiskAndBlurClassSetsAreDisjoint) {
    // 격리 증명의 전제: isRiskClass 와 isBlurClass 는 서로소여야 한다.
    // 이 불변식이 깨지면 개인정보 격리가 즉시 무너진다.
    const veda::ObjectClass all[] = {veda::ObjectClass::Unknown, veda::ObjectClass::Human, veda::ObjectClass::Vehicle,
                                     veda::ObjectClass::Head, veda::ObjectClass::LicensePlate};
    for (auto c : all) {
        EXPECT_FALSE(veda::isRiskClass(c) && veda::isBlurClass(c))
            << "isRiskClass 와 isBlurClass 의 교집합은 반드시 공집합이어야 함";
    }
}

// ============================================================================
// 3. [W2] 벤더 별칭 정규화
// ============================================================================

TEST(RouterTest, W2_VendorAliasesNormalizeToVehicle) {
    const char* aliases[] = {"Car", "car", "truck", "Truck", "bus", "vehicle", "VEHICLE", "motorcycle", "bicycle"};
    for (const char* s : aliases) {
        EXPECT_EQ(veda::objectClassFromString(s), veda::ObjectClass::Vehicle)
            << "\"" << s << "\" 는 Vehicle 로 정규화되어야 함 (미검출 방지)";
    }
}

TEST(RouterTest, W2_VendorAliasesNormalizeToHuman) {
    const char* aliases[] = {"Person", "person", "pedestrian", "Pedestrian", "people", "human", "HUMAN"};
    for (const char* s : aliases) {
        EXPECT_EQ(veda::objectClassFromString(s), veda::ObjectClass::Human)
            << "\"" << s << "\" 는 Human 으로 정규화되어야 함 (미검출 방지)";
    }
}

TEST(RouterTest, W2_BlurAliasesNormalize) {
    EXPECT_EQ(veda::objectClassFromString("face"), veda::ObjectClass::Head);
    EXPECT_EQ(veda::objectClassFromString("plate"), veda::ObjectClass::LicensePlate);
    EXPECT_EQ(veda::objectClassFromString("license_plate"), veda::ObjectClass::LicensePlate);
    EXPECT_EQ(veda::objectClassFromString("licenseplate"), veda::ObjectClass::LicensePlate);
}

TEST(RouterTest, W2_CanonicalNamesStillWork) {
    EXPECT_EQ(veda::objectClassFromString("Human"), veda::ObjectClass::Human);
    EXPECT_EQ(veda::objectClassFromString("Vehicle"), veda::ObjectClass::Vehicle);
    EXPECT_EQ(veda::objectClassFromString("Head"), veda::ObjectClass::Head);
    EXPECT_EQ(veda::objectClassFromString("LicensePlate"), veda::ObjectClass::LicensePlate);
}

TEST(RouterTest, W2_TrulyUnknownStringsRemainUnknown) {
    EXPECT_EQ(veda::objectClassFromString("Bogus"), veda::ObjectClass::Unknown);
    EXPECT_EQ(veda::objectClassFromString(""), veda::ObjectClass::Unknown);
}

TEST(RouterTest, W2_ToStringRemainsCanonicalOnly) {
    // 별칭은 '받아들이는 입력'만 넓힌 것이며, 발행 문자열은 정식 4개로 고정되어야 한다.
    // (toString 이 별칭을 내보내기 시작하면 와이어 포맷이 깨진다)
    EXPECT_EQ(veda::toString(veda::ObjectClass::Human), "Human");
    EXPECT_EQ(veda::toString(veda::ObjectClass::Vehicle), "Vehicle");
    EXPECT_EQ(veda::toString(veda::ObjectClass::Head), "Head");
    EXPECT_EQ(veda::toString(veda::ObjectClass::LicensePlate), "LicensePlate");
}

TEST(RouterTest, W2_AliasedVehicleActuallyReachesRiskStream) {
    // 별칭 정규화의 최종 효과: "Car" 를 보내는 카메라의 차량이 risk 경로에 도달해야 한다.
    ParentBasedRouter router;
    RouteResult out;
    domain::DetectedObject o;
    o.id = 1;
    o.cls = veda::objectClassFromString("Car");  // 벤더 표기
    router.route(makeFrame({o}), out);

    EXPECT_EQ(out.risk.size(), 1u) << "[W2] 벤더 별칭 차량이 risk 경로에서 탈락하면 경보가 울리지 않음";
}

// ============================================================================
// 4. [W1] 출력 파라미터 무할당 & 잔여물 방지
// ============================================================================

TEST(RouterTest, W1_SteadyStateHasZeroHeapAllocations) {
    ParentBasedRouter router;
    RouteResult buf;

    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 64; ++i) {
        objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1),
                                  (i % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Head));
    }
    const auto frame = makeFrame(objs);

    router.route(frame, buf);  // warmup — 여기서만 할당 허용

    g_allocCount = 0;
    g_allocCounting = true;
    for (int i = 0; i < 100; ++i) router.route(frame, buf);
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "[W1] 출력 파라미터 재사용으로 정상 상태 힙 할당이 0이어야 함";
}

TEST(RouterTest, W1_CapacityIsPreservedAcrossCalls) {
    ParentBasedRouter router;
    RouteResult buf;

    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 32; ++i) objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), veda::ObjectClass::Human));
    router.route(makeFrame(objs), buf);

    const std::size_t capRisk = buf.risk.capacity();
    router.route(makeFrame(objs), buf);

    EXPECT_EQ(buf.risk.capacity(), capRisk) << "clear() 는 capacity 를 유지해야 함";
}

TEST(RouterTest, W1_ClearPreventsStaleObjectsFromPreviousFrame) {
    // 버퍼 재사용 설계에서 가장 위험한 회귀: clear() 누락으로 유령 객체가 누적되는 것.
    ParentBasedRouter router;
    RouteResult buf;

    std::vector<domain::DetectedObject> many;
    for (int i = 0; i < 50; ++i) many.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), veda::ObjectClass::Human));
    router.route(makeFrame(many), buf);
    ASSERT_EQ(buf.risk.size(), 50u);

    // 훨씬 작은 프레임을 처리하면 이전 결과가 남아 있으면 안 된다
    router.route(makeFrame({makeObject(999, veda::ObjectClass::Human)}), buf);

    ASSERT_EQ(buf.risk.size(), 1u) << "이전 프레임의 유령 객체가 누적되면 위험 판정이 오염됨";
    EXPECT_EQ(buf.risk[0].id, 999u);
    EXPECT_FALSE(contains(buf.risk, 1)) << "직전 프레임 객체가 남아 있으면 안 됨";
}

TEST(RouterTest, W1_EmptyFrameClearsPreviousResults) {
    ParentBasedRouter router;
    RouteResult buf;

    router.route(makeFrame({makeObject(1, veda::ObjectClass::Human), makeObject(2, veda::ObjectClass::Head)}), buf);
    ASSERT_FALSE(buf.risk.empty());
    ASSERT_FALSE(buf.blur.empty());

    router.route(makeFrame({}), buf);

    EXPECT_TRUE(buf.risk.empty()) << "빈 프레임은 두 스트림을 모두 비워야 함";
    EXPECT_TRUE(buf.blur.empty());
}

// ============================================================================
// 5. 복잡도 특성 (O(N)) — 대량 입력에서도 선형·무크래시
// ============================================================================

TEST(RouterTest, HandlesMaxObjectCountWithoutIssue) {
    ParentBasedRouter router;
    RouteResult buf;

    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 256; ++i) {
        objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1),
                                  (i % 3 == 0) ? veda::ObjectClass::Vehicle
                                               : (i % 3 == 1) ? veda::ObjectClass::Head : veda::ObjectClass::Unknown));
    }

    EXPECT_NO_THROW(router.route(makeFrame(objs), buf));
    // Unknown 은 drop 되므로 합계는 입력보다 작아야 한다
    EXPECT_LT(buf.blur.size() + buf.risk.size(), objs.size());
    EXPECT_GT(buf.blur.size() + buf.risk.size(), 0u);
}
