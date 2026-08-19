#include <QPointF>
#include <cmath>
#include <cstdio>

#include "ui/DigitalTwinHeading.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

/** @brief 두 각도가 같은 방향을 가리키는지 (360도 wraparound를 감안해) 봅니다. */
bool sameHeading(double first, double second) {
    return std::fabs(digitalTwinAngleDifferenceDegrees(first, second)) < 0.001;
}

/**
 * @brief 화면 기준 이동 방향이 아이콘 회전으로 제대로 바뀌는지 검사합니다.
 *
 * @details 아이콘 원본이 위를 보고 있으므로, 오른쪽으로 가면 90도(시계 방향)여야 한다.
 *          scene은 Y가 아래로 자라므로 화면에서 위로 가는 것은 -Y다. 월드 속도를 그대로
 *          넣으면 invertY가 켜진 구성에서 위아래가 뒤집힌다.
 */
void checkHeadingMatchesScreenDirection() {
    const double right = digitalTwinHeadingDegrees(QPointF(1.0, 0.0)) + digitalTwinHeadingOffsetDegrees;
    const double down = digitalTwinHeadingDegrees(QPointF(0.0, 1.0)) + digitalTwinHeadingOffsetDegrees;
    const double left = digitalTwinHeadingDegrees(QPointF(-1.0, 0.0)) + digitalTwinHeadingOffsetDegrees;
    const double up = digitalTwinHeadingDegrees(QPointF(0.0, -1.0)) + digitalTwinHeadingOffsetDegrees;

    check(sameHeading(right, 90.0), "moving right must rotate the up-facing icon a quarter turn clockwise");
    check(sameHeading(down, 180.0), "moving down the screen must rotate the icon half a turn");
    check(sameHeading(left, 270.0), "moving left must rotate the icon three quarters clockwise");
    check(sameHeading(up, 0.0), "moving up the screen must leave the up-facing icon unrotated");

    // 8방향으로 끊던 예전 구현은 여기서 45도로 스냅했다
    const double gentle = digitalTwinHeadingDegrees(QPointF(10.0, 1.0)) + digitalTwinHeadingOffsetDegrees;
    check(!sameHeading(gentle, 90.0) && !sameHeading(gentle, 135.0),
          "a gentle turn must produce a continuous angle, not a snapped one");
    check(std::fabs(digitalTwinAngleDifferenceDegrees(gentle, 90.0)) < 10.0,
          "a gentle turn must stay close to the straight-ahead angle");
}

/** @brief 각도 차이가 360도 경계를 넘어서도 최단 거리로 나오는지 검사합니다. */
void checkAngleDifferenceWrapsAround() {
    check(std::fabs(digitalTwinAngleDifferenceDegrees(1.0, 359.0) - 2.0) < 0.001,
          "crossing zero must report the short way around, not 358 degrees");
    check(std::fabs(digitalTwinAngleDifferenceDegrees(359.0, 1.0) + 2.0) < 0.001,
          "crossing zero backwards must keep the sign of the short way around");
    check(std::fabs(digitalTwinAngleDifferenceDegrees(90.0, 90.0)) < 0.001, "identical angles must differ by zero");
    // -179도는 179도에서 시계 방향으로 2도다. 1도와 359도의 관계와 같아야 한다
    check(std::fabs(digitalTwinAngleDifferenceDegrees(-179.0, 179.0) - 2.0) < 0.001,
          "negative angles must wrap the same way as their positive equivalents");

    // 회전 갱신 문턱(2도)이 방위각 0 근처에서만 다르게 동작하면 안 된다
    check(std::fabs(digitalTwinAngleDifferenceDegrees(0.5, 359.0)) < 2.0,
          "a sub-threshold turn across zero must stay below the update threshold");
}

}  // namespace

int main() {
    checkHeadingMatchesScreenDirection();
    checkAngleDifferenceWrapsAround();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("DigitalTwin heading checks passed\n");
    return 0;
}
