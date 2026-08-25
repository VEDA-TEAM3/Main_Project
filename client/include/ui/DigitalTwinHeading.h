#pragma once

#include <QLineF>
#include <QPointF>

/// 아이콘 원본이 위를 보고 있어 화면 기준 각도에 이만큼 더해야 이동 방향과 맞는다
constexpr double digitalTwinHeadingOffsetDegrees = 90.0;

/**
 * @brief          화면상 이동 벡터를 아이콘 회전 각도로 바꿉니다.
 * @param heading  scene 좌표계 기준 이동 벡터
 * @return         degree 단위 회전 각도
 *
 * @details 월드 좌표가 아니라 scene 좌표를 받는다. invertY가 켜져 있으면 두 축의 부호가
 *          반대라, 월드 속도로 각도를 내면 화면에서 위로 가는 객체의 아이콘이 아래를 본다.
 *          45도 단위 여덟 방향으로 끊으면 완만하게 도는 차량이 방향 사이를 튀어 다니므로
 *          연속 각도를 쓴다.
 *
 *          각도는 std::atan2가 아니라 QLineF::angle()로 낸다. 이 MinGW 구성에서 우리 TU가
 *          std::atan2를 직접 부르면 실행 즉시 32비트 pseudo relocation 실패로 죽는다
 *          (<QtMath>를 include했을 때 죽는 것과 같은 뿌리다). std::fmod도 같고,
 *          std::hypot/std::fabs는 멀쩡하다 — 셋 다 하나씩 넣고 빼서 실측으로 확인했다.
 *          QLineF::angle()은 계산이 Qt 안에서 끝나므로 그 참조가 생기지 않는다.
 *
 *          QLineF::angle()은 y축이 위로 향하는 관례라 atan2(-y, x)를 반환하므로 부호를 뒤집는다.
 */
inline double digitalTwinHeadingDegrees(const QPointF& heading) { return -QLineF(QPointF(0.0, 0.0), heading).angle(); }

/**
 * @brief         두 각도의 최단 차이를 구합니다.
 * @param first   기준 각도(도)
 * @param second  비교 각도(도)
 * @return        -180~180도 범위의 차이
 *
 * @details 단순 뺄셈으로 비교하면 359도와 1도가 358도 차이로 보여, 북쪽을 지나는 순간
 *          미세한 회전이 큰 변화로 잡힌다.
 *
 *          std::fmod는 위 atan2와 같은 이유로 쓸 수 없다. 입력이 각도라 한 바퀴 이상
 *          벌어지지 않으므로 반복으로 접어도 도는 횟수가 사실상 1회다.
 */
inline double digitalTwinAngleDifferenceDegrees(double first, double second) {
    double difference = first - second;
    while (difference > 180.0) {
        difference -= 360.0;
    }
    while (difference < -180.0) {
        difference += 360.0;
    }
    return difference;
}
