#include <QPointF>
#include <cstdio>

#include "model/RiskObjectTracker.h"
#include "ui/DigitalTwinZoneIndex.h"

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
    check(first != nullptr && first->riskLevel == DigitalTwinRiskLevel::Danger,
          "client must preserve the first server risk level");
    check(second != nullptr && second->riskLevel == DigitalTwinRiskLevel::Danger,
          "client must preserve the second server risk level");
}

/** @brief 깜박임 검사용 기본 설정 (grace 200ms + fade-out 180ms = 380ms 유지) */
DigitalTwinRuntimeConfig lifecycleConfig() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;
    config.fadeInMsec = 0;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);
    return config;
}

/** @brief 검사에 쓰는 두 객체 프레임 (100번이 관측 대상) */
RiskFrameData lifecycleFrame(qint64 sourceTimestamp) {
    RiskFrameData frame;
    frame.sourceTimestamp = sourceTimestamp;
    frame.objects = {riskObjectAt(100, 200, 0), riskObjectAt(200, 100, 0)};
    frame.objects[0].worldPosition = QPointF(-40.0, 4.0);
    frame.objects[1].worldPosition = QPointF(-38.0, 4.0);
    return frame;
}

/** @brief 객체가 모두 빠진 새 프레임 */
RiskFrameData emptyFrame(qint64 sourceTimestamp) {
    RiskFrameData frame;
    frame.sourceTimestamp = sourceTimestamp;
    return frame;
}

/**
 * @brief 누락 객체가 grace 구간 동안 밝기를 그대로 유지하고 위험 판단에서는
 * 제외되는지 검사합니다.
 */
void checkMissingObjectGracePeriod() {
    const DigitalTwinRuntimeConfig config = lifecycleConfig();
    check(config.missingGraceMsec == 200, "default grace must be 200ms");
    check(config.fadeOutMsec == 180, "default fade-out must stay 180ms");

    RiskObjectTracker tracker(config);
    tracker.submitFrame(lifecycleFrame(3000), 3000);
    const DigitalTwinSnapshot observedSnapshot = tracker.buildSnapshot(3000);
    check(observedSnapshot.objects.size() == 2, "observed objects must be displayed");
    check(!observedSnapshot.pairRiskStates.isEmpty(), "observed risk pair must remain active");
    const DigitalTwinObject* observedObject = findObject(observedSnapshot, 100);
    check(observedObject != nullptr && observedObject->opacity == 1.0, "observed object must be fully opaque");

    tracker.submitFrame(emptyFrame(3033), 3033);
    const DigitalTwinSnapshot graceSnapshot = tracker.buildSnapshot(3199);
    const DigitalTwinObject* graceObject = findObject(graceSnapshot, 100);
    check(graceObject != nullptr, "missing object must remain during the grace period");
    check(graceObject != nullptr && !graceObject->observed, "grace object must be marked unobserved");
    check(graceObject != nullptr && graceObject->position == QPointF(-40.0, 4.0),
          "grace object must hold its last world position");
    check(graceObject != nullptr && graceObject->opacity == 1.0, "grace object must not be dimmed at all");
    check(graceSnapshot.pairRiskStates.isEmpty(), "grace objects must not extend risk decisions");

    tracker.submitFrame(lifecycleFrame(3200), 3200);
    const DigitalTwinSnapshot restoredSnapshot = tracker.buildSnapshot(3200);
    const DigitalTwinObject* restoredObject = findObject(restoredSnapshot, 100);
    check(restoredObject != nullptr && restoredObject->observed, "same GID must be restored as an observed object");
    check(restoredObject != nullptr && restoredObject->opacity == 1.0, "restored object must keep its opacity");
}

/**
 * @brief grace가 끝난 뒤에만 fade-out이 시작되고, 380ms를 넘겨야 객체가
 * 제거되는지 검사합니다.
 */
void checkMissingObjectFadeOut() {
    const DigitalTwinRuntimeConfig config = lifecycleConfig();
    RiskObjectTracker tracker(config);
    tracker.submitFrame(lifecycleFrame(4000), 4000);
    tracker.buildSnapshot(4000);

    // grace와 fade-out은 이 프레임이 gid를 빠뜨린 시각(4033)에서부터 센다
    tracker.submitFrame(emptyFrame(4033), 4033);

    qreal previousOpacity = 1.0;
    for (qint64 offsetMsec = 233; offsetMsec <= 380; offsetMsec += 33) {
        const DigitalTwinSnapshot fadingSnapshot = tracker.buildSnapshot(4033 + offsetMsec);
        const DigitalTwinObject* fadingObject = findObject(fadingSnapshot, 100);
        check(fadingObject != nullptr, "object must stay in the snapshot until the fade-out ends");
        if (fadingObject == nullptr) {
            return;
        }
        check(fadingObject->opacity < previousOpacity, "fade-out opacity must decrease monotonically");
        previousOpacity = fadingObject->opacity;
    }

    const DigitalTwinSnapshot expiredSnapshot = tracker.buildSnapshot(4414);
    check(findObject(expiredSnapshot, 100) == nullptr, "object missing for more than grace + fade-out must be removed");

    // 안전 관제 화면이므로 처음 보는 gid도 첫 렌더부터 완전히 보여야 한다
    DigitalTwinRuntimeConfig fadeInConfig = lifecycleConfig();
    fadeInConfig.fadeInMsec = 120;
    RiskObjectTracker fadeInTracker(fadeInConfig);
    fadeInTracker.submitFrame(lifecycleFrame(5000), 5000);
    const DigitalTwinSnapshot createdSnapshot = fadeInTracker.buildSnapshot(5000);
    const DigitalTwinObject* createdObject = findObject(createdSnapshot, 100);
    check(createdObject != nullptr && createdObject->opacity == 1.0, "a brand new GID must render fully visible");
}

/**
 * @brief fade-out 도중 같은 gid가 돌아오면 현재 밝기에서 이어서 밝아지는지
 * 검사합니다.
 */
void checkFadeOutRecovery() {
    DigitalTwinRuntimeConfig config = lifecycleConfig();
    config.fadeInMsec = 120;

    // 프레임이 계속 들어오는 정상 구간을 만든다
    RiskObjectTracker tracker(config);
    for (qint64 arrivalMsec = 6000; arrivalMsec <= 6200; arrivalMsec += 100) {
        tracker.submitFrame(lifecycleFrame(arrivalMsec), arrivalMsec);
        tracker.buildSnapshot(arrivalMsec);
    }
    const DigitalTwinSnapshot fullSnapshot = tracker.buildSnapshot(6200);
    const DigitalTwinObject* fullObject = findObject(fullSnapshot, 100);
    check(fullObject != nullptr && fullObject->opacity == 1.0, "observed object must reach full opacity");

    // 새 프레임이 gid를 빠뜨려야 fade-out이 시작된다 (6250 + grace 200 = 6450부터)
    tracker.submitFrame(emptyFrame(6250), 6250);
    const DigitalTwinSnapshot fadingSnapshot = tracker.buildSnapshot(6560);
    const DigitalTwinObject* fadingObject = findObject(fadingSnapshot, 100);
    check(fadingObject != nullptr && fadingObject->opacity < 1.0, "fade-out must have started after the grace period");
    if (fadingObject == nullptr) {
        return;
    }
    const qreal fadedOpacity = fadingObject->opacity;
    check(fadedOpacity > 0.0, "fade-out must not jump straight to zero");

    tracker.submitFrame(lifecycleFrame(6590), 6590);
    const DigitalTwinSnapshot recoveredSnapshot = tracker.buildSnapshot(6590);
    const DigitalTwinObject* recoveredObject = findObject(recoveredSnapshot, 100);
    check(recoveredObject != nullptr && recoveredObject->observed, "recovered GID must be observed again");
    check(recoveredObject != nullptr && recoveredObject->opacity >= fadedOpacity,
          "recovery must resume from the faded opacity instead of zero");
    check(recoveredObject != nullptr && recoveredObject->opacity < 1.0, "recovery must fade in, not snap to full");
}

/**
 * @brief 새 프레임이 오지 않는 동안에도 마지막 승인 프레임의 객체가 그대로
 * 유지되는지 검사합니다.
 *
 * @details 이것이 깜박임의 근원이었다. 배달이 380ms 밀렸다는 이유로 객체를 지우면
 * 같은 gid가 새 객체로 다시 태어난다.
 */
void checkStalledStreamKeepsObjects() {
    RiskObjectTracker tracker(lifecycleConfig());
    tracker.submitFrame(lifecycleFrame(7000), 7000);
    tracker.buildSnapshot(7000);

    // grace + fade-out(380ms)을 넘겨 400ms 동안 프레임이 없어도 화면은 그대로다
    const DigitalTwinSnapshot stalledSnapshot = tracker.buildSnapshot(7400);
    const DigitalTwinObject* stalledObject = findObject(stalledSnapshot, 100);
    check(stalledSnapshot.objects.size() == 2, "a delivery stall must not drop objects");
    check(stalledObject != nullptr && stalledObject->observed, "stalled object must stay observed");
    check(stalledObject != nullptr && stalledObject->opacity == 1.0, "stalled object must keep full opacity");
    check(stalledObject != nullptr && stalledObject->position == QPointF(-40.0, 4.0),
          "stalled object must hold its last world position");

    // 같은 gid가 돌아와도 새 객체가 아니므로 밝기가 리셋되지 않는다
    tracker.submitFrame(lifecycleFrame(7400), 7400);
    const DigitalTwinSnapshot resumedSnapshot = tracker.buildSnapshot(7400);
    const DigitalTwinObject* resumedObject = findObject(resumedSnapshot, 100);
    check(resumedObject != nullptr && resumedObject->observed, "resumed GID must be observed");
    check(resumedObject != nullptr && resumedObject->opacity == 1.0, "resumed GID must not restart from zero");
}

/**
 * @brief 긴 공백 뒤에 도착한 프레임이 gid를 빠뜨렸을 때, grace를 공백 시작이 아니라
 * 그 프레임 도착 시각부터 세는지 검사합니다.
 */
void checkGraceStartsAtTheOmittingFrame() {
    RiskObjectTracker tracker(lifecycleConfig());
    tracker.submitFrame(lifecycleFrame(8000), 8000);
    tracker.buildSnapshot(8000);
    tracker.buildSnapshot(8400);

    tracker.submitFrame(emptyFrame(8400), 8400);
    const DigitalTwinSnapshot graceSnapshot = tracker.buildSnapshot(8560);
    const DigitalTwinObject* graceObject = findObject(graceSnapshot, 100);
    check(graceObject != nullptr, "grace must be measured from the omitting frame, not from the stall");
    check(graceObject != nullptr && graceObject->opacity == 1.0, "grace period must not dim the object");

    const DigitalTwinSnapshot removedSnapshot = tracker.buildSnapshot(8781);
    check(findObject(removedSnapshot, 100) == nullptr, "grace + fade-out after the omitting frame must remove the GID");
}

/** @brief 스트림 전체가 멎으면 frameExpiryMsec 정책으로만 객체가 사라지는지 검사합니다. */
void checkStreamExpiryRemovesEverything() {
    const DigitalTwinRuntimeConfig config = lifecycleConfig();
    RiskObjectTracker tracker(config);
    tracker.submitFrame(lifecycleFrame(9000), 9000);
    tracker.buildSnapshot(9000);

    const qint64 expiryMsec = config.frameExpiryMsec;
    check(!tracker.expireStaleFrame(9000 + expiryMsec, expiryMsec), "frames must survive until the expiry elapses");
    check(!tracker.buildSnapshot(9000 + expiryMsec).objects.isEmpty(), "the last good screen must hold until expiry");
    check(tracker.expireStaleFrame(9001 + expiryMsec, expiryMsec), "an expired stream must be dropped");
    check(!tracker.hasFrame(), "expiry must clear the frame history");
    check(tracker.buildSnapshot(9001 + expiryMsec).objects.isEmpty(), "expiry must remove every object");
}

/** @brief 물리 CCTV 구역을 서버 zoneId로 정하고, 무효할 때만 좌표로 되돌아가는지 검사합니다. */
void checkPhysicalZoneSelection() {
    check(digitalTwinZoneIndex(3, 2, 1) == 0, "zoneId 3 must stay on the first scene even when x says otherwise");
    check(digitalTwinZoneIndex(4, 2, 0) == 1, "zoneId 4 must stay on the second scene even when x says otherwise");
    check(digitalTwinZoneIndex(0, 2, 1) == 0, "zoneId 0..3 must map to the first scene");
    check(digitalTwinZoneIndex(7, 2, 0) == 1, "zoneId 4..7 must map to the second scene");

    check(digitalTwinZoneIndex(-1, 2, 0) == 0, "an unassigned zone must fall back to the world coordinate");
    check(digitalTwinZoneIndex(-1, 2, 1) == 1, "an unassigned zone must fall back to the world coordinate");
    check(digitalTwinZoneIndex(8, 2, 0) == 0, "a zoneId past the configured zones must use the fallback");
    check(digitalTwinZoneIndex(-1, 2, 9) == 1, "the fallback must be clamped to the configured zones");

    // 구역을 늘리면 같은 zoneId가 그대로 뒤쪽 Scene을 가리켜야 한다
    check(digitalTwinZoneIndex(8, 3, 0) == 2, "zoneId 8..11 must map to the third scene once it exists");
    check(digitalTwinZoneIndex(31, 8, 0) == 7, "the eighth zone must accept its own channels");
    check(digitalTwinZoneIndex(32, 8, 3) == 3, "a zoneId past the last zone must still fall back");
}

/**
 * @brief 구역별 월드 상자가 표시 배율을 정하고, 없을 때만 균등 가르기로 돌아가는지
 * 검사합니다.
 */
void checkPhysicalZoneWorldBounds() {
    DigitalTwinWorldConfig halved;
    halved.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);
    halved.zones.resize(2);
    check(halved.zoneBounds(0) == QRectF(-80.0, -40.0, 80.0, 80.0), "zone 0 must fall back to the left half");
    check(halved.zoneBounds(1) == QRectF(0.0, -40.0, 80.0, 80.0), "zone 1 must fall back to the right half");

    // 구역이 늘면 균등 가르기도 그 개수를 따라야 한다
    DigitalTwinWorldConfig quartered = halved;
    quartered.zones.resize(4);
    check(quartered.zoneBounds(0) == QRectF(-80.0, -40.0, 40.0, 80.0), "four zones must split the bounds four ways");
    check(quartered.zoneBounds(3) == QRectF(40.0, -40.0, 40.0, 80.0), "the last zone must end at the right edge");

    // 100m 떨어진 15m짜리 구역 두 개는 균등 가르기로 표현할 수 없다. 창이 서로 붙어 있고
    // 폭도 같아야 하므로, 한쪽을 맞추면 다른 쪽은 지도 끝에 clamp된다
    DigitalTwinWorldConfig zoned = halved;
    zoned.zones = {QRectF(-58.0, -14.0, 17.0, 19.0), QRectF(42.0, -14.0, 17.0, 19.0)};
    check(zoned.zoneBounds(0) == zoned.zones[0], "configured zone 0 box must win over the even split");
    check(zoned.zoneBounds(1) == zoned.zones[1], "configured zone 1 box must win over the even split");
    check(zoned.zoneBounds(-1) == zoned.zones[0] && zoned.zoneBounds(9) == zoned.zones[1],
          "zone index must be clamped instead of reading out of bounds");

    // zoneId가 없을 때 쓰는 좌표 되돌리기. 상자가 멀리 떨어져 있어도 맞는 구역을 골라야 한다
    check(zoned.zoneIndexForWorldX(-50.0) == 0, "a coordinate inside zone 0 must pick zone 0");
    check(zoned.zoneIndexForWorldX(50.0) == 1, "a coordinate inside zone 1 must pick zone 1");
    check(zoned.zoneIndexForWorldX(-70.0) == 0, "a coordinate outside every box must pick the nearest zone");
    check(zoned.zoneIndexForWorldX(70.0) == 1, "a coordinate outside every box must pick the nearest zone");

    // 실측 좌표가 맵을 꽉 채우는지 (반 가르기에서는 폭의 18%만 썼다)
    const QRectF zone = zoned.zoneBounds(0);
    const double narrowSpan = (-42.5 - -56.5) / zone.width();
    const double wideSpan = (-42.5 - -56.5) / halved.zoneBounds(0).width();
    check(narrowSpan > 0.75, "calibrated zone must use most of the map width");
    check(wideSpan < 0.2, "the half split wasted the map, which is what made objects clump");
}
/**
 * @brief 정규화 범위에서 크게 벗어난 좌표가 화면과 자동 경계 계산에서 빠지는지 검사합니다.
 *
 * @details 유한한 이상치는 NaN 검사를 통과하고, 처음 보는 gid는 중앙값 필터와 속도
 *          상한도 우회한다. 그대로 두면 자동 경계가 한 번 벌어진 뒤 되돌아오지 않는다.
 */
void checkOutOfRangeObjectsDropped() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

    RiskFrameData frame;
    frame.sourceTimestamp = 10000;
    frame.objects = {objectAt(1, QPointF(-50.0, 0.0), 0), objectAt(2, QPointF(999999999.0, 0.0), 1),
                     objectAt(3, QPointF(-200.0, 10.0), 2)};

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frame, 10000);
    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(10000);
    check(findObject(snapshot, 1) != nullptr, "an in-range object must survive");
    check(findObject(snapshot, 2) == nullptr, "a finite but absurd coordinate must be dropped, not clamped");
    check(findObject(snapshot, 3) != nullptr, "a coordinate within the calibration margin must survive");
    check(snapshot.objects.size() == 2, "only the out-of-range object may disappear");
}
}  // namespace

/**
 * @brief 위치 보간 구간이 고정 상수가 아니라 실제 수신 간격을 따라가는지 검사합니다.
 *
 * @details 설정값(66ms)만 쓰면 200ms 주기 소스에서 마커가 66ms 만에 목표에 도착해
 *          남은 134ms 동안 멈춘다. 그 정지 구간이 초당 다섯 번 반복되면 눈에 띄게 끊긴다.
 */
void checkPositionTransitionFollowsArrivalInterval() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 66;
    config.fadeInMsec = 0;
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-80.0, -40.0, 160.0, 80.0);

    RiskObjectTracker tracker(config);

    // 200ms 간격으로 일정하게 움직이는 객체를 흘려 수신 간격 평균이 수렴하게 한다.
    // 중앙값 필터가 3샘플을 채워야 목표 좌표가 실제로 움직이기 시작한다
    qint64 arrivalMsec = 1000;
    double worldX = -40.0;
    const auto movingFrame = [&worldX](qint64 sourceTimestamp) {
        RiskFrameData frame;
        frame.sourceTimestamp = sourceTimestamp;
        frame.objects = {objectAt(100, QPointF(worldX, 4.0), 0)};
        return frame;
    };

    for (int index = 0; index < 5; ++index) {
        tracker.submitFrame(movingFrame(arrivalMsec), arrivalMsec);
        tracker.buildSnapshot(arrivalMsec);
        arrivalMsec += 200;
        worldX += 0.5;
    }

    tracker.submitFrame(movingFrame(arrivalMsec), arrivalMsec);

    // buildSnapshot은 값을 돌려주므로 스냅샷을 변수에 묶어 둔다. 임시 객체에 findObject를
    // 걸면 포인터가 그 자리에서 매달린다
    const DigitalTwinSnapshot startSnapshot = tracker.buildSnapshot(arrivalMsec);
    const DigitalTwinSnapshot configWindowSnapshot = tracker.buildSnapshot(arrivalMsec + 66);
    const DigitalTwinSnapshot arrivalWindowSnapshot = tracker.buildSnapshot(arrivalMsec + 200);
    const DigitalTwinSnapshot heldSnapshot = tracker.buildSnapshot(arrivalMsec + 260);

    const DigitalTwinObject* startObject = findObject(startSnapshot, 100);
    const DigitalTwinObject* configWindowObject = findObject(configWindowSnapshot, 100);
    const DigitalTwinObject* arrivalWindowObject = findObject(arrivalWindowSnapshot, 100);
    const DigitalTwinObject* heldObject = findObject(heldSnapshot, 100);

    check(startObject != nullptr && configWindowObject != nullptr && arrivalWindowObject != nullptr &&
              heldObject != nullptr,
          "a moving object must stay in every snapshot");
    if (startObject == nullptr || configWindowObject == nullptr || arrivalWindowObject == nullptr ||
        heldObject == nullptr) {
        return;
    }

    check(startObject->position != arrivalWindowObject->position,
          "the probe object must actually move, otherwise this check proves nothing");
    // 설정값만 쓰면 여기서 이미 목표에 도착해 있다 (= 남은 134ms 동안 정지)
    check(configWindowObject->position != arrivalWindowObject->position,
          "interpolation must still be running at the configured window when the source is slower");
    check(heldObject->position == arrivalWindowObject->position,
          "interpolation must finish exactly when the next sample is due");
}

/** @brief 렌더 보간 프레임과 새 수신 샘플을 지도가 구분할 수 있는지 검사합니다. */
void checkSnapshotCarriesSampleSequence() {
    DigitalTwinRuntimeConfig config = lifecycleConfig();
    config.positionTransitionMsec = 66;

    RiskObjectTracker tracker(config);
    tracker.submitFrame(lifecycleFrame(9000), 9000);
    const qint64 firstSequence = tracker.buildSnapshot(9000).sampleSequence;

    // 새 프레임 없이 다시 그린 스냅샷은 같은 샘플이다 (이동 경로에 점을 남기면 안 된다)
    check(tracker.buildSnapshot(9033).sampleSequence == firstSequence,
          "a render-only snapshot must keep the same sample sequence");

    tracker.submitFrame(lifecycleFrame(9200), 9200);
    check(tracker.buildSnapshot(9200).sampleSequence != firstSequence,
          "a new source frame must advance the sample sequence");
}

int main() {
    checkServerZoneIdPassThrough();
    checkSecondPhysicalCctvObjects();
    checkPhysicalCctvRiskIsolation();
    checkMissingObjectGracePeriod();
    checkMissingObjectFadeOut();
    checkFadeOutRecovery();
    checkStalledStreamKeepsObjects();
    checkGraceStartsAtTheOmittingFrame();
    checkStreamExpiryRemovesEverything();
    checkPhysicalZoneSelection();
    checkPhysicalZoneWorldBounds();
    checkOutOfRangeObjectsDropped();
    checkPositionTransitionFollowsArrivalInterval();
    checkSnapshotCarriesSampleSequence();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskObjectTracker zone checks passed\n");
    return 0;
}
