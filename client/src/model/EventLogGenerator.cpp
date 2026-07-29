#include "model/EventLogGenerator.h"

#include <QHash>
#include <QTime>

namespace {
/**
 * @brief             객체 종류를 이벤트 로그 표시용 한글 이름으로 변환합니다.
 * @param objectType  디지털 트윈 객체 종류
 * @return            객체 종류 표시 문자열
 */
QString eventObjectTypeText(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral("차량");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral("보행자");
    }

    return QStringLiteral("-");
}

/**
 * @brief             객체 종류별 진입/감지 이벤트 문구를 반환합니다.
 * @param objectType  디지털 트윈 객체 종류
 * @return            이벤트 로그 객체 문구
 */
QString detectionEventText(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral("차량 진입");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral("보행자 감지");
    }

    return QStringLiteral("-");
}

/**
 * @brief         구역 판정 구현 전까지 사용할 안정적인 더미 구역명을 반환합니다.
 * @param object  더미 구역을 배정할 객체
 * @return        CH-01~CH-04 중 하나
 */
QString dummyAreaForEventObject(const DigitalTwinObject& object) {
    return QStringLiteral("CH-%1").arg(object.channelIndex + 1, 2, 10, QLatin1Char('0'));
}

/**
 * @brief           디지털 트윈 위험 단계를 이벤트 로그 위험 단계로 변환합니다.
 * @param riskLevel  디지털 트윈 위험 단계
 * @return           이벤트 로그 위험 단계
 */
EventLogRiskLevel eventRiskLevelFromDigitalTwin(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Danger:
            return EventLogRiskLevel::Danger;
        case DigitalTwinRiskLevel::Warning:
            return EventLogRiskLevel::Warning;
        case DigitalTwinRiskLevel::Normal:
            return EventLogRiskLevel::Normal;
    }

    return EventLogRiskLevel::Normal;
}

/**
 * @brief           위험 단계에 맞는 조치 사항을 반환합니다.
 * @param riskLevel  이벤트 로그 위험 단계
 * @return           위험/경고 알림 또는 조치 없음
 */
EventLogAction actionForRiskLevel(EventLogRiskLevel riskLevel) {
    switch (riskLevel) {
        case EventLogRiskLevel::Danger:
            return EventLogAction::DangerAlertActivated;
        case EventLogRiskLevel::Warning:
            return EventLogAction::WarningAlertActivated;
        case EventLogRiskLevel::Normal:
            return EventLogAction::None;
    }

    return EventLogAction::None;
}

/**
 * @brief                 두 객체 ID 조합을 나타내는 안정적인 key를 생성합니다.
 * @param firstObjectId   첫 번째 객체 ID
 * @param secondObjectId  두 번째 객체 ID
 * @return                객체 순서와 무관한 pair key
 */
QString pairKeyForEventObjects(const QString& firstObjectId, const QString& secondObjectId) {
    if (firstObjectId < secondObjectId) {
        return firstObjectId + QStringLiteral("|") + secondObjectId;
    }

    return secondObjectId + QStringLiteral("|") + firstObjectId;
}

/**
 * @brief               객체 쌍을 이벤트 로그 객체 문구로 변환합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @return              예: 보행자-차량, 차량-차량
 */
QString proximityObjectText(const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject) {
    return eventObjectTypeText(firstObject.type) + QStringLiteral("-") + eventObjectTypeText(secondObject.type);
}

/**
 * @brief         객체 진입/감지 로그 항목을 생성합니다.
 * @param object  새로 감지된 객체
 * @return        이벤트 로그 항목
 */
EventLogEntry detectedObjectEntry(const DigitalTwinObject& object) {
    EventLogEntry entry;
    entry.time = QTime::currentTime();
    entry.area = dummyAreaForEventObject(object);
    entry.objectText = detectionEventText(object.type);
    entry.riskLevel = EventLogRiskLevel::Normal;
    entry.action = EventLogAction::None;
    entry.source = EventLogSource::ObjectDetected;
    return entry;
}

/**
 * @brief               객체 간 접근 위험 로그 항목을 생성합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @param riskLevel     현재 접근 위험 단계
 * @return              이벤트 로그 항목
 */
EventLogEntry proximityRiskEntry(const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject,
                                 EventLogRiskLevel riskLevel) {
    EventLogEntry entry;
    entry.time = QTime::currentTime();
    entry.area = dummyAreaForEventObject(firstObject);
    entry.objectText = proximityObjectText(firstObject, secondObject);
    entry.riskLevel = riskLevel;
    entry.action = actionForRiskLevel(riskLevel);
    entry.source = EventLogSource::ObjectProximity;
    return entry;
}
}  // namespace

/**
 * @brief           최신 시뮬레이션 스냅샷에서 새 감지와 접근 위험 상승 이벤트를 생성합니다.
 * @param snapshot  worker가 계산한 객체와 객체 쌍 위험 상태
 * @return          이번 갱신에서 새로 기록할 이벤트 로그 목록
 */
QVector<EventLogEntry> EventLogGenerator::createEntriesFromSnapshot(const DigitalTwinSnapshot& snapshot) {
    QVector<EventLogEntry> entries;
    QSet<QString> currentObjectIds;
    QHash<QString, qsizetype> objectIndexes;
    currentObjectIds.reserve(snapshot.objects.size());
    objectIndexes.reserve(snapshot.objects.size());

    for (qsizetype objectIndex = 0; objectIndex < snapshot.objects.size(); ++objectIndex) {
        const DigitalTwinObject& object = snapshot.objects[objectIndex];
        currentObjectIds.insert(object.objectId);
        objectIndexes.insert(object.objectId, objectIndex);

        if (!knownObjectIds_.contains(object.objectId)) {
            entries.append(detectedObjectEntry(object));
        }
    }

    knownObjectIds_ = currentObjectIds;

    QHash<QString, EventLogRiskLevel> currentPairRiskLevels;
    currentPairRiskLevels.reserve(snapshot.pairRiskStates.size());

    for (const auto& pairRiskState : snapshot.pairRiskStates) {
        const auto firstIndexIterator = objectIndexes.constFind(pairRiskState.firstObjectId);
        const auto secondIndexIterator = objectIndexes.constFind(pairRiskState.secondObjectId);

        if (firstIndexIterator == objectIndexes.cend() || secondIndexIterator == objectIndexes.cend()) {
            continue;
        }

        const EventLogRiskLevel eventRiskLevel = eventRiskLevelFromDigitalTwin(pairRiskState.riskLevel);
        const QString pairKey = pairKeyForEventObjects(pairRiskState.firstObjectId, pairRiskState.secondObjectId);
        const EventLogRiskLevel previousRiskLevel = activePairRiskLevels_.value(pairKey, EventLogRiskLevel::Normal);
        currentPairRiskLevels.insert(pairKey, eventRiskLevel);

        if (previousRiskLevel == eventRiskLevel ||
            (previousRiskLevel == EventLogRiskLevel::Danger && eventRiskLevel == EventLogRiskLevel::Warning)) {
            continue;
        }

        entries.append(proximityRiskEntry(snapshot.objects[firstIndexIterator.value()],
                                          snapshot.objects[secondIndexIterator.value()], eventRiskLevel));
    }

    activePairRiskLevels_ = currentPairRiskLevels;
    return entries;
}

/**
 * @brief   누적된 객체 감지 상태와 접근 위험 상태를 초기화합니다.
 */
void EventLogGenerator::reset() {
    knownObjectIds_.clear();
    activePairRiskLevels_.clear();
}
