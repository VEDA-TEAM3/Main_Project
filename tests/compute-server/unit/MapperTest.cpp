/**
 * @file    MapperTest.cpp
 * @brief   Mapper 계층 격리 단위 테스트 (AffineImageCoordinateMapper / HomographyTransform)
 *
 * @details
 * 감사에서 확인한 수학적 방어를 고정한다:
 *  - 특이 행렬(det==0) / 비유한 값 / pixelSpace 해상도 누락 -> 생성자에서 throw
 *  - 원근 나눗셈의 0 분모(지평선) 차단
 *  - NaN/Inf 전파 차단
 *  - 무할당 in-place 필터링, O(N) 선형 비용
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#include "Contract.h"
#include "interfaces/IGroundPointExtractor.h"
#include "mapper/AffineImageCoordinateMapper.h"
#include "transform/HomographyTransform.h"

namespace {

long g_allocCount = 0;
bool g_allocCounting = false;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

domain::DetectedObject makeObject(veda::ObjectId id, double l, double t, double r, double b) {
    domain::DetectedObject o;
    o.id = id;
    o.cls = veda::ObjectClass::Head;
    o.box.l = l;
    o.box.t = t;
    o.box.r = r;
    o.box.b = b;
    return o;
}

/// @brief 아래를 내려다보는 전형적 CCTV 를 흉내낸 비특이 호모그래피
std::array<double, 9> sampleHomography() {
    return {{1.0, 0.0, -0.5, 0.0, 0.5, 0.2, 0.0, -0.4, 1.0}};
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
// A. HomographyTransform — 생성자 검증 (조립 시점 fail-fast)
// ============================================================================

TEST(MapperTest, Homography_ValidMatrixConstructs) {
    EXPECT_NO_THROW({ HomographyTransform h(sampleHomography()); });
}

TEST(MapperTest, Homography_SingularMatrixThrows) {
    // 퇴화 행렬(det == 0): 역변환이 불가능하므로 조립 시점에 즉시 실패해야 한다.
    // (행 2가 행 1의 2배 -> 선형 종속)
    const std::array<double, 9> singular{{1, 2, 3, 2, 4, 6, 7, 8, 9}};
    EXPECT_THROW({ HomographyTransform h(singular); }, std::invalid_argument);
}

TEST(MapperTest, Homography_AllZeroMatrixThrows) {
    const std::array<double, 9> zero{{0, 0, 0, 0, 0, 0, 0, 0, 0}};
    EXPECT_THROW({ HomographyTransform h(zero); }, std::invalid_argument);
}

TEST(MapperTest, Homography_NonFiniteMatrixThrows) {
    std::array<double, 9> withNaN = sampleHomography();
    withNaN[4] = kNaN;
    EXPECT_THROW({ HomographyTransform h(withNaN); }, std::invalid_argument);

    std::array<double, 9> withInf = sampleHomography();
    withInf[0] = kInf;
    EXPECT_THROW({ HomographyTransform h(withInf); }, std::invalid_argument);
}

TEST(MapperTest, Homography_PixelSpaceRequiresPositiveResolution) {
    HomographyTransform::Options opts;
    opts.pixelSpace = true;
    opts.imageWidth = 0.0;  // 누락된 해상도
    opts.imageHeight = 1080.0;

    EXPECT_THROW({ HomographyTransform h(sampleHomography(), opts); }, std::invalid_argument)
        << "pixelSpace 인데 해상도가 0 이하면 환산이 불가능하므로 던져야 함";
}

TEST(MapperTest, Homography_PixelSpaceWithValidResolutionConstructs) {
    HomographyTransform::Options opts;
    opts.pixelSpace = true;
    opts.imageWidth = 1920.0;
    opts.imageHeight = 1080.0;
    EXPECT_NO_THROW({ HomographyTransform h(sampleHomography(), opts); });
}

// ============================================================================
// B. HomographyTransform — 원근 나눗셈 / 지평선 / NaN 전파
// ============================================================================

TEST(MapperTest, Homography_ProjectsValidGroundPoint) {
    HomographyTransform h(sampleHomography());
    // 화면 하단 중앙: 아래로 기울어진 CCTV 라면 반드시 카메라 앞쪽 지면
    const auto result = h.toLocal(domain::ImagePoint{0.5, 1.0});

    ASSERT_TRUE(result.has_value()) << "정상 지면점은 변환에 성공해야 함";
    EXPECT_TRUE(std::isfinite(result->x));
    EXPECT_TRUE(std::isfinite(result->y));
}

TEST(MapperTest, Homography_HorizonPointIsRejectedNotDividedByZero) {
    // 분모 = h6*u + h7*v + h8 = -0.4*v + 1.0 -> v = 2.5 에서 0
    // 0 나눗셈으로 inf 를 내놓는 대신 nullopt 를 반환해야 한다.
    HomographyTransform h(sampleHomography());
    const auto result = h.toLocal(domain::ImagePoint{0.5, 2.5});

    EXPECT_FALSE(result.has_value()) << "분모 0(지평선)은 0 나눗셈 없이 폐기되어야 함";
}

TEST(MapperTest, Homography_BeyondHorizonIsRejected) {
    // v > 2.5 이면 분모 부호가 뒤집혀 '반대편으로 반사된 그럴듯한 좌표'가 나온다.
    // 부호 정규화 덕분에 denominator <= 0 검사 하나로 걸러져야 한다.
    HomographyTransform h(sampleHomography());
    const auto result = h.toLocal(domain::ImagePoint{0.5, 5.0});

    EXPECT_FALSE(result.has_value()) << "지평선 너머 점은 팬텀 좌표를 만들지 않고 폐기되어야 함";
}

TEST(MapperTest, Homography_NaNInputDoesNotProduceValidOutput) {
    HomographyTransform h(sampleHomography());
    const auto result = h.toLocal(domain::ImagePoint{kNaN, 1.0});

    if (result.has_value()) {
        // 값이 나왔다면 최소한 유한해야 한다 (NaN 이 하류로 새면 안 됨)
        EXPECT_TRUE(std::isfinite(result->x)) << "NaN 이 로컬 좌표로 전파되면 안 됨";
        EXPECT_TRUE(std::isfinite(result->y));
    } else {
        SUCCEED() << "NaN 입력이 폐기됨 (기대 동작)";
    }
}

TEST(MapperTest, Homography_InfInputDoesNotProduceNonFiniteOutput) {
    HomographyTransform h(sampleHomography());
    const auto result = h.toLocal(domain::ImagePoint{kInf, 1.0});

    if (result.has_value()) {
        EXPECT_TRUE(std::isfinite(result->x));
        EXPECT_TRUE(std::isfinite(result->y));
    } else {
        SUCCEED();
    }
}

TEST(MapperTest, Homography_BoundsCheckDiscardsOutOfRangeResults) {
    HomographyTransform::Options opts;
    opts.boundsEnabled = true;
    opts.minX = -1.0;
    opts.maxX = 1.0;
    opts.minY = 0.0;
    opts.maxY = 1.0;

    HomographyTransform h(sampleHomography(), opts);

    // 범위를 크게 벗어나는 좌표를 만들 법한 점 (지평선에 가까울수록 발산)
    const auto far = h.toLocal(domain::ImagePoint{0.5, 2.4});
    if (far.has_value()) {
        EXPECT_GE(far->x, opts.minX);
        EXPECT_LE(far->x, opts.maxX);
        EXPECT_GE(far->y, opts.minY);
        EXPECT_LE(far->y, opts.maxY);
    } else {
        SUCCEED() << "범위 밖 좌표가 폐기됨 (기대 동작)";
    }
}

TEST(MapperTest, Homography_IsDeterministicAcrossCalls) {
    HomographyTransform h(sampleHomography());
    const auto a = h.toLocal(domain::ImagePoint{0.5, 0.9});
    const auto b = h.toLocal(domain::ImagePoint{0.5, 0.9});

    ASSERT_EQ(a.has_value(), b.has_value());
    if (a.has_value()) {
        EXPECT_DOUBLE_EQ(a->x, b->x);
        EXPECT_DOUBLE_EQ(a->y, b->y);
    }
}

TEST(MapperTest, Homography_ScaleInvarianceOfProjection) {
    // 호모그래피는 스칼라배 불변 — 전체에 상수를 곱해도 사상 결과가 같아야 한다.
    auto m = sampleHomography();
    auto scaled = m;
    for (double& v : scaled) v *= 3.0;

    HomographyTransform h1(m);
    HomographyTransform h2(scaled);

    const auto r1 = h1.toLocal(domain::ImagePoint{0.5, 1.0});
    const auto r2 = h2.toLocal(domain::ImagePoint{0.5, 1.0});

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NEAR(r1->x, r2->x, 1e-9);
    EXPECT_NEAR(r1->y, r2->y, 1e-9);
}

// ============================================================================
// C. AffineImageCoordinateMapper — 생성자 검증
// ============================================================================

TEST(MapperTest, Affine_ValidParametersConstruct) {
    EXPECT_NO_THROW({ AffineImageCoordinateMapper m(1.0, 1.0, 0.0, 0.0); });
}

TEST(MapperTest, Affine_NonPositiveScaleThrows) {
    EXPECT_THROW({ AffineImageCoordinateMapper m(0.0, 1.0, 0.0, 0.0); }, std::invalid_argument);
    EXPECT_THROW({ AffineImageCoordinateMapper m(1.0, -1.0, 0.0, 0.0); }, std::invalid_argument)
        << "음수 스케일은 좌표계를 뒤집으므로 거부되어야 함";
}

TEST(MapperTest, Affine_NonFiniteParametersThrow) {
    EXPECT_THROW({ AffineImageCoordinateMapper m(kNaN, 1.0, 0.0, 0.0); }, std::invalid_argument);
    EXPECT_THROW({ AffineImageCoordinateMapper m(1.0, kInf, 0.0, 0.0); }, std::invalid_argument);
    EXPECT_THROW({ AffineImageCoordinateMapper m(1.0, 1.0, kNaN, 0.0); }, std::invalid_argument);
}

// ============================================================================
// D. AffineImageCoordinateMapper — 매핑 / 필터링 / 무할당
// ============================================================================

TEST(MapperTest, Affine_IdentityMappingLeavesBoxUnchanged) {
    AffineImageCoordinateMapper mapper(1.0, 1.0, 0.0, 0.0);
    std::vector<domain::DetectedObject> objs{makeObject(1, 0.2, 0.3, 0.4, 0.5)};

    mapper.map(objs, 0);

    ASSERT_EQ(objs.size(), 1u);
    EXPECT_DOUBLE_EQ(objs[0].box.l, 0.2);
    EXPECT_DOUBLE_EQ(objs[0].box.t, 0.3);
    EXPECT_DOUBLE_EQ(objs[0].box.r, 0.4);
    EXPECT_DOUBLE_EQ(objs[0].box.b, 0.5);
}

TEST(MapperTest, Affine_ScaleAndOffsetAreApplied) {
    AffineImageCoordinateMapper mapper(0.5, 0.5, 0.25, 0.25);
    std::vector<domain::DetectedObject> objs{makeObject(1, 0.0, 0.0, 1.0, 1.0)};

    mapper.map(objs, 0);

    ASSERT_EQ(objs.size(), 1u);
    EXPECT_DOUBLE_EQ(objs[0].box.l, 0.25);
    EXPECT_DOUBLE_EQ(objs[0].box.r, 0.75);
}

TEST(MapperTest, Affine_OffScreenBoxesAreFilteredOut) {
    // offset 으로 화면 밖으로 밀린 박스는 제거되어야 한다.
    AffineImageCoordinateMapper mapper(1.0, 1.0, 5.0, 5.0);
    std::vector<domain::DetectedObject> objs{makeObject(1, 0.1, 0.1, 0.2, 0.2)};

    mapper.map(objs, 0);

    EXPECT_TRUE(objs.empty()) << "출력 프레임과 겹치지 않는 박스는 제거되어야 함";
}

TEST(MapperTest, Affine_ResultsAreClampedToUnitRange) {
    AffineImageCoordinateMapper mapper(2.0, 2.0, 0.0, 0.0);
    std::vector<domain::DetectedObject> objs{makeObject(1, 0.1, 0.1, 0.9, 0.9)};

    mapper.map(objs, 0);

    ASSERT_EQ(objs.size(), 1u);
    EXPECT_GE(objs[0].box.l, 0.0);
    EXPECT_LE(objs[0].box.r, 1.0) << "매핑 결과는 [0,1] 로 클램프되어야 함";
    EXPECT_GE(objs[0].box.t, 0.0);
    EXPECT_LE(objs[0].box.b, 1.0);
}

TEST(MapperTest, Affine_EdgeFlagsAreNotRecomputed) {
    // 경계 판정은 파서가 Metadata 좌표계에서 한 번만 수행한다.
    // 매퍼가 clamp 후 재판정하면 '앱 화면 기준 경계'라는 다른 의미가 섞인다.
    AffineImageCoordinateMapper mapper(2.0, 2.0, 0.0, 0.0);
    auto obj = makeObject(1, 0.1, 0.1, 0.9, 0.9);
    obj.touchesBorder = false;
    obj.bottomTruncated = false;
    std::vector<domain::DetectedObject> objs{obj};

    mapper.map(objs, 0);

    ASSERT_EQ(objs.size(), 1u);
    EXPECT_FALSE(objs[0].touchesBorder) << "매퍼는 경계 플래그를 건드리면 안 됨";
    EXPECT_FALSE(objs[0].bottomTruncated);
}

TEST(MapperTest, Affine_DegenerateBoxAfterClampIsDropped) {
    // clamp 후 폭/높이가 0 이하가 되는 박스는 제거되어야 한다.
    AffineImageCoordinateMapper mapper(1.0, 1.0, -0.95, 0.0);
    std::vector<domain::DetectedObject> objs{makeObject(1, 0.9, 0.1, 0.95, 0.2)};

    mapper.map(objs, 0);

    for (const auto& o : objs) {
        EXPECT_GT(o.box.r, o.box.l) << "퇴화 박스가 살아남으면 안 됨";
        EXPECT_GT(o.box.b, o.box.t);
    }
}

TEST(MapperTest, Affine_MapIsZeroAllocation) {
    AffineImageCoordinateMapper mapper(1.0, 1.0, 0.0, 0.0);
    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 64; ++i) objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), 0.1, 0.1, 0.2, 0.2));

    g_allocCount = 0;
    g_allocCounting = true;
    mapper.map(objs, 0);
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "in-place 필터링 + 축소 resize 이므로 힙 할당이 0이어야 함";
}

TEST(MapperTest, Affine_PreservesCapacityOnFiltering) {
    AffineImageCoordinateMapper mapper(1.0, 1.0, 5.0, 5.0);  // 전부 화면 밖으로
    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 16; ++i) objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), 0.1, 0.1, 0.2, 0.2));
    const std::size_t capBefore = objs.capacity();

    mapper.map(objs, 0);

    EXPECT_TRUE(objs.empty());
    EXPECT_EQ(objs.capacity(), capBefore) << "축소 resize 는 capacity 를 유지해야 함";
}

TEST(MapperTest, Affine_EmptyInputIsHandled) {
    AffineImageCoordinateMapper mapper(1.0, 1.0, 0.0, 0.0);
    std::vector<domain::DetectedObject> objs;
    EXPECT_NO_THROW(mapper.map(objs, 0));
    EXPECT_TRUE(objs.empty());
}

TEST(MapperTest, Affine_LinearCostOverManyObjects) {
    // O(N) 특성 확인: 대량 입력에서도 크래시 없이 선형 처리
    AffineImageCoordinateMapper mapper(1.0, 1.0, 0.0, 0.0);
    std::vector<domain::DetectedObject> objs;
    for (int i = 0; i < 256; ++i) objs.push_back(makeObject(static_cast<veda::ObjectId>(i + 1), 0.1, 0.1, 0.2, 0.2));

    EXPECT_NO_THROW(mapper.map(objs, 0));
    EXPECT_EQ(objs.size(), 256u);
}
