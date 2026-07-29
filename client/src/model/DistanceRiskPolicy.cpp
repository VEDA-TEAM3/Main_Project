#include <cmath>

#include "model/DigitalTwinRiskPolicy.h"

namespace {
constexpr double dangerDistanceThreshold = 0.08;
constexpr double warningDistanceThreshold = 0.16;

/**
 * @brief               두 객체가 모두 보행자인지 확인합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @return              둘 다 보행자이면 true
 */
bool isPedestrianPair(const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject) {
    return firstObject.type == DigitalTwinObjectType::Pedestrian &&
           secondObject.type == DigitalTwinObjectType::Pedestrian;
}
}  // namespace

/**
 * @brief               두 객체 사이의 유클리드 거리를 기준으로 위험 단계를 계산합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @return              현재 거리 기준 위험 단계
 */
DigitalTwinRiskLevel DistanceRiskPolicy::riskLevelForObjects(const DigitalTwinObject& firstObject,
                                                             const DigitalTwinObject& secondObject) const {
    if (isPedestrianPair(firstObject, secondObject)) {
        return DigitalTwinRiskLevel::Normal;
    }

    const double xDistance = firstObject.position.x() - secondObject.position.x();
    const double yDistance = firstObject.position.y() - secondObject.position.y();
    const double distance = std::hypot(xDistance, yDistance);

    if (distance <= dangerDistanceThreshold) {
        return DigitalTwinRiskLevel::Danger;
    }

    if (distance <= warningDistanceThreshold) {
        return DigitalTwinRiskLevel::Warning;
    }

    return DigitalTwinRiskLevel::Normal;
}
