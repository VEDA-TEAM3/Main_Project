#include <QPointF>
#include <cstdio>

#include "model/RiskObjectTracker.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

RiskObjectData objectAt(qint64 globalId, const QPointF& worldPosition, int zoneId) {
    RiskObjectData object;
    object.globalId = globalId;
    object.objectClass = QStringLiteral("Human");
    object.worldPosition = worldPosition;
    object.zoneId = zoneId;
    return object;
}

const DigitalTwinObject* findObject(const DigitalTwinSnapshot& snapshot, qint64 globalId) {
    const QString objectId = QStringLiteral("G-%1").arg(globalId);
    for (const DigitalTwinObject& object : snapshot.objects) {
        if (object.objectId == objectId) {
            return &object;
        }
    }
    return nullptr;
}

RiskObjectData riskObjectAt(qint64 globalId, qint64 nearestId, int zoneId) {
    RiskObjectData object = objectAt(globalId, QPointF(0.0, 0.0), zoneId);
    object.objectClass = QStringLiteral("Vehicle");
    object.nearestId = nearestId;
    object.riskLevel = DigitalTwinRiskLevel::Danger;
    return object;
}

/**
 * @brief 서버의 zoneId와 월드 좌표가 클라이언트 재계산 없이 유지되는지
 * 검사합니다.
 */
void checkServerZoneIdPassThrough() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 66;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

    RiskFrameData frame;
    frame.sourceTimestamp = 1000;
    frame.objects = {objectAt(1, QPointF(-50.0, 0.0), 0), objectAt(2, QPointF(-50.0, 0.0), 7),
                     objectAt(3, QPointF(50.0, 0.0), -1)};

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frame, 1000);
    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(1000);

    const DigitalTwinObject* first = findObject(snapshot, 1);
    const DigitalTwinObject* second = findObject(snapshot, 2);
    const DigitalTwinObject* unassigned = findObject(snapshot, 3);
    check(first != nullptr && first->channelIndex == 0, "zoneId 0 must map to channel index 0");
    check(second != nullptr && second->channelIndex == 7, "zoneId 7 must map to channel index 7");
    check(unassigned != nullptr && unassigned->channelIndex == -1, "unassigned zone must stay unassigned");
    check(first != nullptr && first->position == QPointF(-50.0, 0.0), "world position must remain unchanged");
    check(second != nullptr && second->position == QPointF(-50.0, 0.0),
          "identical coordinates must not force identical channels");

    frame.sourceTimestamp = 1033;
    frame.objects[0].worldPosition = QPointF(-45.0, 5.0);
    tracker.submitFrame(frame, 1033);
    tracker.buildSnapshot(1033);
    const DigitalTwinSnapshot movedSnapshot = tracker.buildSnapshot(1099);
    const DigitalTwinObject* moved = findObject(movedSnapshot, 1);
    check(moved != nullptr && moved->position == QPointF(-45.0, 5.0),
          "position transition must preserve world coordinates");
}

/**
 * @brief 서로 다른 물리 CCTV 구역의 객체가 하나의 위험 쌍으로 결합되지 않는지 검사합니다.
 */
void checkPhysicalCctvRiskIsolation() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;

    RiskFrameData frame;
    frame.sourceTimestamp = 2000;
    frame.objects = {riskObjectAt(10, 20, 0), riskObjectAt(20, 10, 4)};

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frame, 2000);
    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(2000);

    const DigitalTwinObject* first = findObject(snapshot, 10);
    const DigitalTwinObject* second = findObject(snapshot, 20);
    check(snapshot.pairRiskStates.isEmpty(), "different physical CCTV maps must not share pair risk");
    check(tracker.takeRiskEvents().isEmpty(), "different physical CCTV maps must not share pulse events");
    check(first != nullptr && first->riskLevel == DigitalTwinRiskLevel::Normal,
          "cross-map risk must not activate the first map");
    check(second != nullptr && second->riskLevel == DigitalTwinRiskLevel::Normal,
          "cross-map risk must not activate the second map");
}
}  // namespace

int main() {
    checkServerZoneIdPassThrough();
    checkPhysicalCctvRiskIsolation();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskObjectTracker zone checks passed\n");
    return 0;
}
