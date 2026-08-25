/**
 * @file    MetricTest.cpp
 * @brief   EuclideanMetric 격리 단위 테스트 (hypot -> sqrt 교체 회귀 방지)
 *
 * @details
 * 감사에서 확인한 항목을 고정한다:
 *  - sqrt 치환이 hypot 과 동일한 값을 내는지 (정확도 회귀 방지)
 *  - 거리 함수의 수학적 성질 (동일성/대칭성/삼각부등식) -- 융합의 클러스터링이 이를 전제한다
 *  - 무상태 const: 호출당 힙 할당 0, 락 없이 공유 가능
 *  - 비유한 입력은 '막지 않는다'는 계약 (방어는 O(N) 상류 책임)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "metric/EuclideanMetric.h"

namespace {

long g_allocCount = 0;
bool g_allocCounting = false;

domain::WorldPoint pt(double x, double y) {
    domain::WorldPoint p;
    p.x = x;
    p.y = y;
    return p;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

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
// 1. 거리 정확도 (sqrt 치환 검증)
// ============================================================================

TEST(MetricTest, PythagoreanTripleIsExact) {
    // 3-4-5 는 double 로 정확히 표현되므로 오차 허용 없이 비교할 수 있다.
    EuclideanMetric m;
    EXPECT_DOUBLE_EQ(m.calculate(pt(0.0, 0.0), pt(3.0, 4.0)), 5.0);
    EXPECT_DOUBLE_EQ(m.calculate(pt(1.0, 1.0), pt(4.0, 5.0)), 5.0);
    EXPECT_DOUBLE_EQ(m.calculate(pt(0.0, 0.0), pt(5.0, 12.0)), 13.0);
}

TEST(MetricTest, AxisAlignedDistances) {
    EuclideanMetric m;
    EXPECT_DOUBLE_EQ(m.calculate(pt(0.0, 0.0), pt(7.0, 0.0)), 7.0);
    EXPECT_DOUBLE_EQ(m.calculate(pt(0.0, 0.0), pt(0.0, 7.0)), 7.0);
    EXPECT_DOUBLE_EQ(m.calculate(pt(2.0, 3.0), pt(-5.0, 3.0)), 7.0);
}

TEST(MetricTest, SqrtMatchesHypotAcrossDomainRange) {
    // [ 이 테스트가 지키는 것 ]
    // std::hypot 을 std::sqrt(dx*dx+dy*dy) 로 바꾼 것은 O(N^2) 경로를 5.17배 단축한
    // 성능 패치다. 포기한 것은 극단값 스케일링 보호인데, worldBounds 안의 미터 좌표에서는
    // 두 함수의 결과가 구분되지 않아야 한다. 구분된다면 교체 근거가 무너진 것이다.
    EuclideanMetric m;
    for (double x = -1000.0; x <= 1000.0; x += 37.5) {
        for (double y = -1000.0; y <= 1000.0; y += 91.25) {
            const double expected = std::hypot(x, y);
            const double actual = m.calculate(pt(0.0, 0.0), pt(x, y));
            EXPECT_NEAR(actual, expected, 1e-9) << "x=" << x << " y=" << y << " 에서 hypot 과 어긋남";
        }
    }
}

TEST(MetricTest, LargeCoordinatesDoNotOverflow) {
    // worldBounds 상한(약 1e4 m)의 100배를 넣어도 dx*dx 는 double 상한에서 한참 멀다.
    EuclideanMetric m;
    const double d = m.calculate(pt(-1e6, -1e6), pt(1e6, 1e6));
    EXPECT_TRUE(std::isfinite(d)) << "도메인 범위에서 오버플로가 나면 sqrt 치환 근거가 무너진다";
    EXPECT_NEAR(d, std::hypot(2e6, 2e6), 1e-3);
}

// ============================================================================
// 2. 거리 함수의 수학적 성질
// ============================================================================

TEST(MetricTest, IdentityOfIndiscernibles) {
    EuclideanMetric m;
    EXPECT_DOUBLE_EQ(m.calculate(pt(0.0, 0.0), pt(0.0, 0.0)), 0.0);
    EXPECT_DOUBLE_EQ(m.calculate(pt(-3.5, 9.25), pt(-3.5, 9.25)), 0.0);
}

TEST(MetricTest, Symmetry) {
    EuclideanMetric m;
    const auto a = pt(-4.5, 2.25);
    const auto b = pt(11.0, -7.75);
    EXPECT_DOUBLE_EQ(m.calculate(a, b), m.calculate(b, a));
}

TEST(MetricTest, TriangleInequality) {
    // ConcatFuser/GridFuser 의 클러스터링이 이 성질을 전제한다.
    // 다른 거리 함수로 교체할 때 반드시 유지되어야 하는 계약이다.
    EuclideanMetric m;
    const auto a = pt(0.0, 0.0);
    const auto b = pt(3.0, 1.0);
    const auto c = pt(7.0, -2.0);
    EXPECT_LE(m.calculate(a, c), m.calculate(a, b) + m.calculate(b, c) + 1e-12);
}

TEST(MetricTest, NeverReturnsNegative) {
    EuclideanMetric m;
    EXPECT_GE(m.calculate(pt(5.0, 5.0), pt(-5.0, -5.0)), 0.0);
    EXPECT_GE(m.calculate(pt(-1e-9, 0.0), pt(1e-9, 0.0)), 0.0);
}

// ============================================================================
// 3. 무상태성 (statelessness)
// ============================================================================

TEST(MetricTest, RepeatedCallsAreDeterministic) {
    // 내부 상태(캐시/카운터)가 생기면 같은 입력에 다른 값이 나오거나
    // 스레드 안전성이 깨진다. 값이 흔들리지 않는지부터 고정한다.
    EuclideanMetric m;
    const double first = m.calculate(pt(1.5, -2.5), pt(-3.5, 4.5));
    for (int i = 0; i < 1000; ++i) {
        EXPECT_DOUBLE_EQ(m.calculate(pt(1.5, -2.5), pt(-3.5, 4.5)), first);
    }
}

TEST(MetricTest, InterleavedCallsDoNotInfluenceEachOther) {
    EuclideanMetric m;
    const double a = m.calculate(pt(0.0, 0.0), pt(3.0, 4.0));
    m.calculate(pt(100.0, 100.0), pt(-100.0, -100.0));  // 사이에 다른 호출
    const double b = m.calculate(pt(0.0, 0.0), pt(3.0, 4.0));
    EXPECT_DOUBLE_EQ(a, b) << "직전 호출이 결과에 영향을 주면 무상태가 아니다";
}

TEST(MetricTest, IsCallableThroughConstInterfaceReference) {
    // calculate() 가 const 가 아니면 컴파일되지 않는다.
    // 무상태 const 는 락 없이 공유하기 위한 전제 조건이다.
    const EuclideanMetric m;
    const IDistanceMetric& iface = m;
    EXPECT_DOUBLE_EQ(iface.calculate(pt(0.0, 0.0), pt(6.0, 8.0)), 10.0);
}

TEST(MetricTest, ConcurrentCallsAreSafeAndConsistent) {
    // 락 없이 여러 스레드가 같은 인스턴스를 공유해도 값이 흔들리지 않아야 한다.
    // 실패 카운터 같은 가변 멤버가 추가되면 이 테스트가 흔들린다.
    const EuclideanMetric m;
    constexpr int kThreads = 4;
    constexpr int kIters = 20000;
    std::vector<std::thread> workers;
    std::vector<int> mismatches(kThreads, 0);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&m, &mismatches, t] {
            for (int i = 0; i < kIters; ++i) {
                if (m.calculate(pt(0.0, 0.0), pt(3.0, 4.0)) != 5.0) ++mismatches[static_cast<std::size_t>(t)];
            }
        });
    }
    for (auto& w : workers) w.join();

    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(mismatches[static_cast<std::size_t>(t)], 0) << "스레드 " << t << " 에서 값이 흔들림";
    }
}

// ============================================================================
// 4. 무할당 (호출당 힙 할당 0)
// ============================================================================

TEST(MetricTest, CalculateDoesNotAllocate) {
    EuclideanMetric m;
    const auto a = pt(1.0, 2.0);
    const auto b = pt(3.0, 4.0);
    m.calculate(a, b);  // warmup

    g_allocCount = 0;
    g_allocCounting = true;
    volatile double sink = 0.0;
    for (int i = 0; i < 10000; ++i) {
        sink = sink + m.calculate(a, b);
    }
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "거리 계산은 O(N^2) 경로다 -- 호출당 할당이 생기면 즉시 증폭된다";
}

// ============================================================================
// 5. 비유한 입력 계약 (막지 않는다 -- 방어는 상류 책임)
// ============================================================================

TEST(MetricTest, NonFiniteInputPropagatesRatherThanBeingFiltered) {
    // [ 이것은 결함이 아니라 계약이다 ]
    // 유한성 검사를 이 계층(O(N^2))에 넣으면 같은 방어를 N배 비싸게 산다.
    // 방어는 O(N) 인 ThresholdRiskPolicy 1단계와 변환 계층이 담당한다.
    // 이 테스트는 "여기서 막고 있다"고 오해한 채 상류 검사를 지우는 것을 방지한다.
    EuclideanMetric m;
    EXPECT_TRUE(std::isnan(m.calculate(pt(kNaN, 0.0), pt(1.0, 1.0))));
    EXPECT_TRUE(std::isnan(m.calculate(pt(0.0, kNaN), pt(1.0, 1.0))));
    EXPECT_TRUE(std::isinf(m.calculate(pt(kInf, 0.0), pt(0.0, 0.0))));
}

TEST(MetricTest, NaNResultFailsEveryComparison) {
    // NaN 이 왜 '조용한 무경보'가 되는지를 고정한다.
    // 두 비교가 모두 false 이므로 경보도 안 나가고 병합도 안 된다.
    EuclideanMetric m;
    const double d = m.calculate(pt(kNaN, 0.0), pt(1.0, 1.0));
    EXPECT_FALSE(d < 5.0) << "warningDistance 판정이 false -> 경보 없음";
    EXPECT_FALSE(d > 5.0) << "dedup/track 판정도 false -> 병합도 안 됨";
}
