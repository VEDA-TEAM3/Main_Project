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
    frame.objects[0].worldPosition = QPointF(-49.9, 0.1);
    tracker.submitFrame(frame, 1033);
    tracker.buildSnapshot(1033);
    const DigitalTwinSnapshot movedSnapshot = tracker.buildSnapshot(1099);
    const DigitalTwinObject* moved = findObject(movedSnapshot, 1);
    check(moved != nullptr && moved->position == QPointF(-49.9, 0.1),
          "position transition must preserve world coordinates");
}

/**
 * @brief 서버의 8개 zoneId와 양수 X 월드 좌표가 추적기에서 손실되지 않는지
 * 확인합니다.
 */
void checkSecondPhysicalCctvObjects() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

    RiskFrameData frame;
    frame.sourceTimestamp = 1500;
    for (int zoneId = 0; zoneId < 8; ++zoneId) {
        const double x = zoneId < 4 ? -50.0 : 50.0;
        frame.objects.append(objectAt(zoneId + 1, QPointF(x, zoneId - 3.5), zoneId));
    }

    RiskObjectTracker tracker(config);
    check(tracker.submitFrame(frame, 1500), "eight-channel frame must be accepted");
    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(1500);
    check(snapshot.objects.size() == 8, "objects from both physical CCTV regions must be retained");

    for (int zoneId = 0; zoneId < 8; ++zoneId) {
        const DigitalTwinObject* object = findObject(snapshot, zoneId + 1);
        check(object != nullptr, "every server GID must produce a tracked object");
        check(object != nullptr && object->channelIndex == zoneId, "zoneId 0..7 must pass through unchanged");
        if (zoneId >= 4) {
            check(object != nullptr && object->position.x() > 0.0,
                  "second physical CCTV object must keep its positive world X "
                  "coordinate");
        }
    }
}

/**
 * @brief 서로 다른 물리 CCTV 구역의 객체가 하나의 위험 쌍으로 결합되지 않는지
 * 검사합니다.
 */
void checkPhysicalCctvRiskIsolation() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

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

/**
 * @brief 누락 객체가 100ms 동안만 유지되고 위험 판단에서는 제외되는지
 * 검사합니다.
 */
void checkMissingObjectGracePeriod() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;
    config.fadeInMsec = 0;
    config.missingGraceMsec = 100;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

    RiskFrameData frame;
    frame.sourceTimestamp = 3000;
    frame.objects = {riskObjectAt(100, 200, 0), riskObjectAt(200, 100, 0)};
    frame.objects[0].worldPosition = QPointF(-40.0, 4.0);
    frame.objects[1].worldPosition = QPointF(-38.0, 4.0);

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frame, 3000);
    const DigitalTwinSnapshot observedSnapshot = tracker.buildSnapshot(3000);
    check(observedSnapshot.objects.size() == 2, "observed objects must be displayed");
    check(!observedSnapshot.pairRiskStates.isEmpty(), "observed risk pair must remain active");

    RiskFrameData emptyFrame;
    emptyFrame.sourceTimestamp = 3033;
    tracker.submitFrame(emptyFrame, 3033);
    const DigitalTwinSnapshot graceSnapshot = tracker.buildSnapshot(3050);
    const DigitalTwinObject* graceObject = findObject(graceSnapshot, 100);
    check(graceObject != nullptr, "missing object must remain during the 100ms grace period");
    check(graceObject != nullptr && !graceObject->observed, "grace object must be marked unobserved");
    check(graceObject != nullptr && graceObject->position == QPointF(-40.0, 4.0),
          "grace object must hold its last world position");
    check(graceObject != nullptr && graceObject->opacity < 1.0, "grace object must be visually dimmed");
    check(graceSnapshot.pairRiskStates.isEmpty(), "grace objects must not extend risk decisions");

    frame.sourceTimestamp = 3066;
    tracker.submitFrame(frame, 3066);
    const DigitalTwinSnapshot restoredSnapshot = tracker.buildSnapshot(3066);
    const DigitalTwinObject* restoredObject = findObject(restoredSnapshot, 100);
    check(restoredObject != nullptr && restoredObject->observed, "same GID must be restored as an observed object");

    RiskObjectTracker expiryTracker(config);
    frame.sourceTimestamp = 4000;
    expiryTracker.submitFrame(frame, 4000);
    expiryTracker.buildSnapshot(4000);
    emptyFrame.sourceTimestamp = 4033;
    expiryTracker.submitFrame(emptyFrame, 4033);
    const DigitalTwinSnapshot expiredSnapshot = expiryTracker.buildSnapshot(4101);
    check(findObject(expiredSnapshot, 100) == nullptr, "object missing for more than 100ms must be removed");
}
}  // namespace

int main() {
    checkServerZoneIdPassThrough();
    checkSecondPhysicalCctvObjects();
    checkPhysicalCctvRiskIsolation();
    checkMissingObjectGracePeriod();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskObjectTracker zone checks passed\n");
    return 0;
}
