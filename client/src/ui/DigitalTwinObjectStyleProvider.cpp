#include "ui/DigitalTwinObjectStyleProvider.h"

namespace {
/**
 * @brief         객체 타입과 위험 단계에 맞는 아이콘 리소스 경로를 반환합니다.
 * @param object  스타일을 계산할 디지털 트윈 객체
 * @return        Qt resource icon path
 */
QString iconPathForObject(const DigitalTwinObject& object) {
    if (object.riskLevel == DigitalTwinRiskLevel::Danger) {
        switch (object.type) {
            case DigitalTwinObjectType::Vehicle:
                return QStringLiteral(":/icons/vehicle_danger.png");
            case DigitalTwinObjectType::Pedestrian:
                return QStringLiteral(":/icons/human_danger.png");
        }
    }

    if (object.riskLevel == DigitalTwinRiskLevel::Warning) {
        switch (object.type) {
            case DigitalTwinObjectType::Vehicle:
                return QStringLiteral(":/icons/vehicle_warning.png");
            case DigitalTwinObjectType::Pedestrian:
                return QStringLiteral(":/icons/human_warning.png");
        }
    }

    switch (object.type) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral(":/icons/vehicle.png");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral(":/icons/human.png");
    }

    return QStringLiteral(":/icons/vehicle.png");
}

/**
 * @brief            객체 타입별 기본 아이콘 표시 크기를 반환합니다.
 * @param objectType  디지털 트윈 객체 타입
 * @return            scene에 표시할 아이콘 크기
 */
QSize iconSizeForType(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QSize(92, 92);
        case DigitalTwinObjectType::Pedestrian:
            return QSize(62, 62);
    }

    return QSize(44, 44);
}

/**
 * @brief           위험 단계에 맞는 라벨 색상을 반환합니다.
 * @param riskLevel  객체의 현재 위험 단계
 * @return           라벨에 사용할 색상
 */
QColor labelColorForRiskLevel(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Normal:
            return QColor(QStringLiteral("#d8e3f2"));
        case DigitalTwinRiskLevel::Warning:
            return QColor(QStringLiteral("#ffd43b"));
        case DigitalTwinRiskLevel::Danger:
            return QColor(QStringLiteral("#ff5a5f"));
    }

    return QColor(QStringLiteral("#d8e3f2"));
}

/**
 * @brief             객체 유형별 이동 경로 색상을 반환합니다.
 * @param objectType  디지털 트윈 객체 유형
 * @return            데모와 실제 TopView에 공통 적용할 이동 경로 색상
 */
QColor trailColorForObjectType(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QColor(QStringLiteral("#23d8ff"));
        case DigitalTwinObjectType::Pedestrian:
            return QColor(QStringLiteral("#45f23a"));
    }

    return QColor(QStringLiteral("#23d8ff"));
}

/**
 * @brief         아이콘 리소스 로딩 실패 시 대체로 그릴 색상을 반환합니다.
 * @param object  스타일을 계산할 디지털 트윈 객체
 * @return        fallback shape에 사용할 색상
 */
QColor fallbackColorForObject(const DigitalTwinObject& object) {
    if (object.riskLevel == DigitalTwinRiskLevel::Danger) {
        return QColor(QStringLiteral("#ff4d4f"));
    }

    if (object.riskLevel == DigitalTwinRiskLevel::Warning) {
        return QColor(QStringLiteral("#ffd43b"));
    }

    return object.color;
}
}  // namespace

/**
 * @brief         객체 타입과 위험 단계에 맞는 아이콘/색상 스타일을 제공합니다.
 * @param object  스타일을 조회할 디지털 트윈 객체
 * @return        화면 표시용 스타일 값
 */
DigitalTwinObjectVisualStyle DefaultDigitalTwinObjectStyleProvider::styleFor(const DigitalTwinObject& object) const {
    return {iconPathForObject(object), iconSizeForType(object.type), labelColorForRiskLevel(object.riskLevel),
            trailColorForObjectType(object.type), fallbackColorForObject(object)};
}
