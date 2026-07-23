/**
 * @file    verify_world.cpp
 * @brief   로컬(정규화 이미지) 좌표 -> 월드 좌표 변환 검증 하네스
 *
 * @details
 * HomographyTransform / BottomCenterExtractor 를 실제 소스 그대로 링크해서 돌린다.
 * 현장 캘리브레이션 값을 넣고 좌표가 맞는지 확인하는 용도이자, 회귀 방지용 체크.
 *
 * @par [ 빌드 & 실행 ]
 * @code
 * cmake -B build && cmake --build build --target verify-world
 * ./build/verify-world
 * @endcode
 *
 * @par [ 현장 값으로 검증하는 법 ]
 * kImgW/kImgH 를 카메라 해상도로, kCalib 을 실측 4점 대응
 * (이미지 픽셀 좌표 <-> 도면상 월드 좌표 m)으로 교체한 뒤 실행한다.
 * 여기서 뽑힌 H_pixel 을 config.json 의 homography 에 그대로 넣고
 * homographySpace 를 "pixel" 로 두면 된다.
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "ground/BottomCenterExtractor.h"
#include "transform/HomographyTransform.h"

namespace {

int g_pass = 0;
int g_fail = 0;

// 테스트 자체 출력은 C stdio(printf)로 통일한다.
// LogSilencer 가 std::cout/std::cerr 를 잠시 끊어버리므로, 여기서 iostream 을 쓰면
// 검증 결과까지 같이 사라짐
void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

void section(const std::string& title) { std::printf("\n=== %s ===\n", title.c_str()); }

/**
 * @brief   테스트 대상 코드가 남기는 로그를 잠시 삼키는 RAII 가드
 * @details HomographyTransform 은 실패 사유를 logError 로 남기는데, 여기서는 실패를
 *          '기대하는' 케이스가 많아서 그대로 두면 출력이 뒤섞임
 */
class LogSilencer {
public:
    LogSilencer() : out_(std::cout.rdbuf(nullptr)), err_(std::cerr.rdbuf(nullptr)) {}
    ~LogSilencer() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    LogSilencer(const LogSilencer&) = delete;
    LogSilencer& operator=(const LogSilencer&) = delete;

private:
    std::streambuf* out_;
    std::streambuf* err_;
};

/// @brief 이미지 픽셀 좌표 <-> 월드 좌표(m) 대응 한 쌍
struct Correspondence {
    double u, v;  ///< 이미지 좌표 (픽셀)
    double x, y;  ///< 월드 좌표 (m)
};

/**
 * @brief   4점 대응으로 호모그래피를 푸는 DLT (OpenCV findHomography 와 동일한 결과)
 * @details h8 = 1 로 고정하고 미지수 h0..h7 에 대한 8x8 선형계를 가우스 소거로 푼다
 */
std::array<double, 9> solveHomography(const std::array<Correspondence, 4>& points) {
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const auto& p = points[i];
        double* rowX = a[2 * i];
        rowX[0] = p.u;
        rowX[1] = p.v;
        rowX[2] = 1;
        rowX[6] = -p.x * p.u;
        rowX[7] = -p.x * p.v;
        rowX[8] = p.x;

        double* rowY = a[2 * i + 1];
        rowY[3] = p.u;
        rowY[4] = p.v;
        rowY[5] = 1;
        rowY[6] = -p.y * p.u;
        rowY[7] = -p.y * p.v;
        rowY[8] = p.y;
    }

    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
                pivot = row;
        }
        for (int c = 0; c < 9; ++c)
            std::swap(a[col][c], a[pivot][c]);

        const double diagonal = a[col][col];
        for (int c = col; c < 9; ++c)
            a[col][c] /= diagonal;

        for (int row = 0; row < 8; ++row) {
            if (row == col)
                continue;
            const double factor = a[row][col];
            for (int c = col; c < 9; ++c)
                a[row][c] -= factor * a[col][c];
        }
    }

    std::array<double, 9> h{};
    for (int i = 0; i < 8; ++i)
        h[i] = a[i][8];
    h[8] = 1.0;
    return h;
}

/// @name 현장 값으로 교체할 부분
/// @{
constexpr double kImgW = 1920.0;
constexpr double kImgH = 1080.0;

/// @brief 주차장 CCTV 지면 영역 4점 (사다리꼴)
const std::array<Correspondence, 4> kCalib{{
    {300.0, 1000.0, -5.0, 3.0},  ///< 근거리 좌
    {1620.0, 1000.0, 5.0, 3.0},  ///< 근거리 우
    {700.0, 620.0, -5.0, 15.0},  ///< 원거리 좌
    {1220.0, 620.0, 5.0, 15.0},  ///< 원거리 우
}};
/// @}

double errorMeters(const veda::LocalPoint& got, double x, double y) { return std::hypot(got.x - x, got.y - y); }

}  // namespace

int main() {
    std::printf("compute-server 로컬->월드 좌표 변환 검증\n");
    std::printf("이미지 %.0fx%.0f, 4점 캘리브레이션 (DLT)\n", kImgW, kImgH);

    const std::array<double, 9> hPixel = solveHomography(kCalib);
    std::printf("\nH_pixel (row-major) — config.json 의 homography 에 그대로 넣을 값:\n");
    for (int r = 0; r < 3; ++r)
        std::printf("  % .6e  % .6e  % .6e\n", hPixel[r * 3], hPixel[r * 3 + 1], hPixel[r * 3 + 2]);

    LogSilencer silencer;

    // -----------------------------------------------------------------------
    section("C-2  homographySpace=\"pixel\" 환산이 맞는가");
    // -----------------------------------------------------------------------
    HomographyTransform::Options pixelOptions;
    pixelOptions.pixelSpace = true;
    pixelOptions.imageWidth = kImgW;
    pixelOptions.imageHeight = kImgH;
    HomographyTransform pixelTransform(hPixel, pixelOptions);

    double worstError = 0.0;
    for (const auto& p : kCalib) {
        const auto world = pixelTransform.toLocal(domain::ImagePoint{p.u / kImgW, p.v / kImgH});
        if (!world) {
            check(false, "캘리브레이션 점이 변환 실패함");
            continue;
        }
        worstError = std::max(worstError, errorMeters(*world, p.x, p.y));
    }
    std::printf("      4개 대응점 최대 오차: %.3e m\n", worstError);
    check(worstError < 1e-9, "픽셀 공간 행렬 + pixelSpace=true -> 캘리브레이션 점 완전 복원");

    // -----------------------------------------------------------------------
    section("C-2  같은 행렬을 normalized 로 잘못 선언하면?");
    // -----------------------------------------------------------------------
    HomographyTransform wrongTransform(hPixel, HomographyTransform::Options{});
    double wrongErrorSum = 0.0;
    int wrongAccepted = 0;
    for (const auto& p : kCalib) {
        const auto world = wrongTransform.toLocal(domain::ImagePoint{p.u / kImgW, p.v / kImgH});
        if (world) {
            ++wrongAccepted;
            wrongErrorSum += errorMeters(*world, p.x, p.y);
            std::printf("      기대(%6.1f,%6.1f)m  실제(%10.3f,%10.3f)m  오차 %.1fm\n", p.x, p.y, world->x, world->y,
                        errorMeters(*world, p.x, p.y));
        } else {
            std::printf("      기대(%6.1f,%6.1f)m  -> 변환 거부\n", p.x, p.y);
        }
    }
    const double wrongAverage = wrongAccepted > 0 ? wrongErrorSum / wrongAccepted : -1.0;
    std::printf("      평균 오차: %.1f m (통과한 점 %d개)\n", wrongAverage, wrongAccepted);
    check(wrongAccepted < 4 || wrongAverage > 1.0, "좌표계를 틀리면 예외 없이 수십 m 오차 -> 설정 명시가 필수");

    // -----------------------------------------------------------------------
    section("C-3  지평선 처리");
    // -----------------------------------------------------------------------
    const double horizonPx = -(hPixel[6] * 960.0 + hPixel[8]) / hPixel[7];
    std::printf("      화면 중앙(u=960)의 지평선 v = %.1f px\n", horizonPx);

    struct HorizonCase {
        double v;
        bool shouldReject;
        const char* label;
    };
    const HorizonCase horizonCases[] = {
        {horizonPx - 60.0, true, "지평선보다 60px 위 (하늘/건물 영역)"},
        {horizonPx - 1.0, true, "지평선 바로 위"},
        {horizonPx + 40.0, false, "지평선보다 40px 아래 (먼 지면)"},
        {1000.0, false, "근거리 지면"},
    };
    for (const auto& c : horizonCases) {
        const auto world = pixelTransform.toLocal(domain::ImagePoint{960.0 / kImgW, c.v / kImgH});
        if (world)
            std::printf("      v=%7.1f px -> (%9.2f, %9.2f)m\n", c.v, world->x, world->y);
        else
            std::printf("      v=%7.1f px -> 거부\n", c.v);
        check(!world.has_value() == c.shouldReject,
              std::string(c.label) + (c.shouldReject ? " -> 거부되어야 함" : " -> 통과해야 함"));
    }

    // -----------------------------------------------------------------------
    section("C-3  행렬 부호가 뒤집혀도 동일한가 (스칼라배 불변성)");
    // -----------------------------------------------------------------------
    std::array<double, 9> hNegated = hPixel;
    for (double& value : hNegated)
        value = -value;
    HomographyTransform negatedTransform(hNegated, pixelOptions);

    double negatedWorst = 0.0;
    bool negatedAllAccepted = true;
    for (const auto& p : kCalib) {
        const auto world = negatedTransform.toLocal(domain::ImagePoint{p.u / kImgW, p.v / kImgH});
        if (!world) {
            negatedAllAccepted = false;
            continue;
        }
        negatedWorst = std::max(negatedWorst, errorMeters(*world, p.x, p.y));
    }
    check(negatedAllAccepted && negatedWorst < 1e-9, "-H 를 줘도 부호 정규화 덕에 결과가 동일");
    check(!negatedTransform.toLocal(domain::ImagePoint{960.0 / kImgW, (horizonPx - 60.0) / kImgH}).has_value(),
          "-H 를 줘도 지평선 위는 여전히 거부");

    // -----------------------------------------------------------------------
    section("C-4  bbox 아래변 잘림이 월드 좌표에 주는 오차");
    // -----------------------------------------------------------------------
    BottomCenterExtractor ground;

    const domain::NormBox actualBox{0.45, 0.72, 0.55, 0.93};    ///< 발끝이 보이는 정상 케이스
    const domain::NormBox truncatedBox{0.45, 0.72, 0.55, 1.0};  ///< 하단이 잘려 b가 1.0으로 기록된 케이스

    const auto actualWorld = pixelTransform.toLocal(ground.extract(actualBox));
    const auto truncatedWorld = pixelTransform.toLocal(ground.extract(truncatedBox));
    if (actualWorld && truncatedWorld) {
        const double gap = std::hypot(actualWorld->x - truncatedWorld->x, actualWorld->y - truncatedWorld->y);
        std::printf("      실제 발끝 b=0.93 -> (%.2f, %.2f)m\n", actualWorld->x, actualWorld->y);
        std::printf("      잘린   발끝 b=1.00 -> (%.2f, %.2f)m\n", truncatedWorld->x, truncatedWorld->y);
        std::printf("      위치 오차: %.2f m\n", gap);
        check(gap > 0.5, "아래변 잘림이 0.5m 이상 오차를 유발 -> dropBottomTruncated 정책의 근거");
    } else {
        check(false, "잘림 시나리오 변환 실패");
    }

    const domain::NormBox sideCutBox{0.0, 0.0, 0.10, 0.93};  ///< 좌/상단만 잘리고 발은 보이는 케이스
    check(pixelTransform.toLocal(ground.extract(sideCutBox)).has_value(),
          "좌/상단만 잘린 객체는 지면점이 유효 -> dropAnyEdge 가 기본값이면 안 되는 이유");

    // -----------------------------------------------------------------------
    section("C-3  월드 범위 검사");
    // -----------------------------------------------------------------------
    HomographyTransform::Options boundsOptions = pixelOptions;
    boundsOptions.boundsEnabled = true;
    boundsOptions.minX = -10.0;
    boundsOptions.maxX = 10.0;
    boundsOptions.minY = 0.0;
    boundsOptions.maxY = 25.0;
    HomographyTransform boundedTransform(hPixel, boundsOptions);

    check(boundedTransform.toLocal(domain::ImagePoint{960.0 / kImgW, 1000.0 / kImgH}).has_value(),
          "범위 안 지면점은 통과");

    const domain::ImagePoint nearHorizon{960.0 / kImgW, (horizonPx + 2.0) / kImgH};
    if (const auto diverged = pixelTransform.toLocal(nearHorizon)) {
        std::printf("      지평선+2px 지면점 = (%.1f, %.1f)m (범위검사 없으면 그대로 발행됨)\n", diverged->x,
                    diverged->y);
    }
    check(!boundedTransform.toLocal(nearHorizon).has_value(), "지평선 근처 발산 좌표는 범위 검사로 폐기");

    section("결과");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
