#include <QRandomGenerator>
#include <QString>

#include "model/DigitalTwinObjectSpawner.h"

namespace {
constexpr int minimumSpawnDelayMsec = 2000;
constexpr int maximumSpawnDelayMsec = 5000;
constexpr double initialMinX = 0.14;
constexpr double initialMaxX = 0.86;
constexpr double spawnerObjectMinY = 0.14;
constexpr double spawnerObjectMaxY = 0.86;
constexpr double edgeEntryPadding = 0.02;
constexpr double entryLeftX = -edgeEntryPadding;
constexpr double entryRightX = 1.0 + edgeEntryPadding;
constexpr double spawnerMinimumHorizontalVelocity = 0.0045;
constexpr double spawnerMaximumHorizontalVelocity = 0.0085;
constexpr double spawnerMaximumVerticalVelocity = 0.0022;

/**
 * @brief               지정 범위 안의 난수를 생성합니다.
 * @param minimumValue  최소값
 * @param maximumValue  최대값
 * @return              범위 안의 임의 실수
 */
double spawnerRandomRange(double minimumValue, double maximumValue) {
    return minimumValue + (maximumValue - minimumValue) * QRandomGenerator::global()->generateDouble();
}

/**
 * @brief   데모 객체 타입을 무작위로 선택합니다.
 * @return  차량 또는 보행자 중 하나
 */
DigitalTwinObjectType randomObjectType() {
    switch (QRandomGenerator::global()->bounded(2)) {
        case 0:
            return DigitalTwinObjectType::Vehicle;
        default:
            return DigitalTwinObjectType::Pedestrian;
    }
}

/**
 * @brief             객체 타입별 기본 표시 색상을 반환합니다.
 * @param objectType  디지털 트윈 객체 타입
 * @return            이동 경로와 fallback에 사용할 기본 색상
 */
QColor colorForObjectType(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QColor(QStringLiteral("#23d8ff"));
        case DigitalTwinObjectType::Pedestrian:
            return QColor(QStringLiteral("#45f23a"));
    }

    return QColor(QStringLiteral("#23d8ff"));
}

/**
 * @brief             객체 타입별 ID prefix를 반환합니다.
 * @param objectType  디지털 트윈 객체 타입
 * @return            ID prefix 문자열
 */
QString idPrefixForObjectType(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral("V");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral("P");
    }

    return QStringLiteral("O");
}
}  // namespace

/**
 * @brief              시작 시 맵 내부에 배치할 데모 객체를 생성합니다.
 * @param objectCount  생성할 초기 객체 수
 * @return             초기 객체 목록
 */
QVector<DigitalTwinObject> RandomEdgeObjectSpawner::createInitialObjects(int objectCount) {
    QVector<DigitalTwinObject> objects;
    objects.reserve(objectCount);

    for (int i = 0; i < objectCount; ++i) {
        const DigitalTwinObjectType objectType = randomObjectType();
        const bool movingRight = QRandomGenerator::global()->bounded(2) == 0;
        const double horizontalVelocity =
            spawnerRandomRange(spawnerMinimumHorizontalVelocity, spawnerMaximumHorizontalVelocity);
        const QPointF position(spawnerRandomRange(initialMinX, initialMaxX),
                               spawnerRandomRange(spawnerObjectMinY, spawnerObjectMaxY));
        const QPointF velocity(movingRight ? horizontalVelocity : -horizontalVelocity,
                               spawnerRandomRange(-spawnerMaximumVerticalVelocity, spawnerMaximumVerticalVelocity));

        objects.append(createObject(objectType, position, velocity));
    }

    return objects;
}

/**
 * @brief   좌우 맵 경계선 바깥에서 안쪽으로 진입하는 객체를 생성합니다.
 * @return  새로 입장할 객체
 */
DigitalTwinObject RandomEdgeObjectSpawner::createEnteringObject() {
    const DigitalTwinObjectType objectType = randomObjectType();
    const bool enterFromLeft = QRandomGenerator::global()->bounded(2) == 0;
    const double horizontalVelocity =
        spawnerRandomRange(spawnerMinimumHorizontalVelocity, spawnerMaximumHorizontalVelocity);
    const QPointF position(enterFromLeft ? entryLeftX : entryRightX,
                           spawnerRandomRange(spawnerObjectMinY, spawnerObjectMaxY));
    const QPointF velocity(enterFromLeft ? horizontalVelocity : -horizontalVelocity,
                           spawnerRandomRange(-spawnerMaximumVerticalVelocity, spawnerMaximumVerticalVelocity));

    return createObject(objectType, position, velocity);
}

/**
 * @brief   다음 객체 생성까지의 지연 시간을 무작위로 반환합니다.
 * @return  4~8초 사이 지연 시간(ms)
 */
int RandomEdgeObjectSpawner::nextSpawnDelayMsec() const {
    return QRandomGenerator::global()->bounded(minimumSpawnDelayMsec, maximumSpawnDelayMsec + 1);
}

/**
 * @brief             공통 객체 필드를 채워 디지털 트윈 객체를 생성합니다.
 * @param objectType  객체 타입
 * @param position    정규화 좌표계 기준 초기 위치
 * @param velocity    tick당 이동 벡터
 * @return            생성된 디지털 트윈 객체
 */
DigitalTwinObject RandomEdgeObjectSpawner::createObject(DigitalTwinObjectType objectType, const QPointF& position,
                                                        const QPointF& velocity) {
    DigitalTwinObject object;
    object.objectId = nextObjectId(objectType);
    object.type = objectType;
    object.position = position;
    object.velocity = velocity;
    object.color = colorForObjectType(objectType);
    object.riskLevel = DigitalTwinRiskLevel::Normal;

    return object;
}

/**
 * @brief             객체 타입 prefix와 증가 번호로 고유 ID를 생성합니다.
 * @param objectType  객체 타입
 * @return            화면 표시와 pair key에 사용할 객체 ID
 */
QString RandomEdgeObjectSpawner::nextObjectId(DigitalTwinObjectType objectType) {
    const QString objectId =
        QStringLiteral("%1-%2").arg(idPrefixForObjectType(objectType)).arg(nextSequence_, 3, 10, QLatin1Char('0'));
    ++nextSequence_;

    return objectId;
}
