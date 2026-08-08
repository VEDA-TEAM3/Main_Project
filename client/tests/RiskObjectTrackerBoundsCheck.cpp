// 자동 월드 경계 정규화 self-check.
// 빌드: cmake --preset debug-ninja -DQTCCTV_BUILD_CHECKS=ON 후
//       build/debug-ninja/risk_object_tracker_bounds_check 실행 (성공 시 종료 코드 0)

#include <QPointF>
#include <QtGlobal>
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

DigitalTwinRuntimeConfig checkConfig() {
    DigitalTwinRuntimeConfig config;
    config.positionTransitionMsec = 0;  // 보간을 끄고 정규화 결과만 본다
    config.world.fixedBoundsEnabled = false;
    config.world.automaticBoundsWarmupMsec = 0;
    config.world.automaticBoundsMinimumSamples = 2;
    config.world.automaticBoundsPaddingRatio = 0.05;  // 0으로 두면 확장 직후 좌표가 경계선 위에 정확히 놓인다
    config.world.automaticBoundsOutlierFraction = 0.0;
    return config;
}

RiskObjectData objectAt(qint64 globalId, const QPointF& worldPosition) {
    RiskObjectData object;
    object.globalId = globalId;
    object.objectClass = QStringLiteral("Human");
    object.worldPosition = worldPosition;
    return object;
}

RiskFrameData frameAt(qint64 sourceTimestamp, const QVector<RiskObjectData>& objects) {
    RiskFrameData frame;
    frame.sourceTimestamp = sourceTimestamp;
    frame.objects = objects;
    return frame;
}

QPointF positionOf(const DigitalTwinSnapshot& snapshot, qint64 globalId) {
    const QString objectId = QStringLiteral("G-%1").arg(globalId);
    for (const DigitalTwinObject& object : snapshot.objects) {
        if (object.objectId == objectId) {
            return object.position;
        }
    }
    return QPointF(-1.0, -1.0);
}

/// 관측 범위가 최소 폭보다 좁으면 데이터가 경계 중심에 놓여야 한다 (구석 쏠림 회귀 방지).
void checkNarrowObservationStaysCentered() {
    RiskObjectTracker tracker(checkConfig());
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(12.0, 8.0)), objectAt(2, QPointF(12.0, 8.0))}), 1000);

    const QPointF position = positionOf(tracker.buildSnapshot(1000), 1);
    check(qAbs(position.x() - 0.5) < 0.01, "narrow observation must normalize to the centre on X");
    check(qAbs(position.y() - 0.5) < 0.01, "narrow observation must normalize to the centre on Y");
}

/// 경계 밖 좌표는 가장자리로 잘리지 않고 경계를 넓혀야 한다 (고정 경계 클램프 회귀 방지).
void checkOutOfRangePositionExpandsBounds() {
    RiskObjectTracker tracker(checkConfig());
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(0.0, 0.0)), objectAt(2, QPointF(2.0, 2.0))}), 1000);
    tracker.buildSnapshot(1000);

    // 경계 확장은 연속 관측을 요구하고, 확장 후 배율 변화는 속도 상한을 거쳐 반영된다
    qint64 timeMsec = 1000;
    for (int step = 0; step < 10; ++step) {
        timeMsec += 100;
        tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(1.0, 1.0)), objectAt(3, QPointF(60.0, 60.0))}),
                            timeMsec);
        tracker.buildSnapshot(timeMsec);
    }

    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(timeMsec);
    const QPointF farPosition = positionOf(snapshot, 3);
    const QPointF nearPosition = positionOf(snapshot, 1);
    check(farPosition.x() < 0.999 && farPosition.x() > 0.001, "out-of-range X must not clamp to the map edge");
    check(farPosition.y() < 0.999 && farPosition.y() > 0.001, "out-of-range Y must not clamp to the map edge");
    check(qAbs(farPosition.x() - nearPosition.x()) > 0.5, "expanded bounds must keep far and near objects apart");
}
/// 한 프레임짜리 순간 이동은 속도 상한까지만 반영되고, 되돌아오면 원위치여야 한다.
void checkSingleFrameTeleportIsRateLimited() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;  // 경계 확장을 배제하고 필터만 본다
    config.world.bounds = QRectF(0.0, 0.0, 100.0, 100.0);

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(10.0, 10.0))}), 1000);
    const double settledX = positionOf(tracker.buildSnapshot(1000), 1).x();
    check(qAbs(settledX - 0.1) < 0.01, "first sample must be shown as measured");

    // 80m 순간 이동이지만 100ms 동안 허용되는 이동은 2.5m뿐이다
    tracker.submitFrame(frameAt(1100, {objectAt(1, QPointF(90.0, 10.0))}), 1100);
    const double jumpedX = positionOf(tracker.buildSnapshot(1100), 1).x();
    check(jumpedX < 0.15, "one-frame teleport must not cross the map");

    tracker.submitFrame(frameAt(1200, {objectAt(1, QPointF(10.0, 10.0))}), 1200);
    const double recoveredX = positionOf(tracker.buildSnapshot(1200), 1).x();
    check(qAbs(recoveredX - 0.1) < 0.02, "returning to the real position must not lag behind");
}

/// 실제 이동은 이상치와 달리 연속으로 들어오므로 몇 프레임 안에 따라잡아야 한다.
void checkSustainedMovementCatchesUp() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(0.0, 0.0, 100.0, 100.0);

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(10.0, 10.0))}), 1000);
    tracker.buildSnapshot(1000);

    // 80m를 속도 상한(8m/s)으로 따라잡으려면 10초가 필요하다
    qint64 timeMsec = 1000;
    for (int step = 0; step < 105; ++step) {
        timeMsec += 100;
        tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(90.0, 10.0))}), timeMsec);
        tracker.buildSnapshot(timeMsec);
    }

    const double followedX = positionOf(tracker.buildSnapshot(timeMsec), 1).x();
    check(qAbs(followedX - 0.9) < 0.01, "sustained movement must reach the measured position");
}
/// 한 프레임만 튄 좌표는 화면에 전혀 반영되지 않아야 한다.
void checkIsolatedOutlierFrameIsRejected() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(0.0, 0.0, 100.0, 100.0);

    RiskObjectTracker tracker(config);
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(10.0, 10.0))}), 1000);
    tracker.buildSnapshot(1000);
    tracker.submitFrame(frameAt(1100, {objectAt(1, QPointF(10.0, 10.0))}), 1100);
    tracker.buildSnapshot(1100);

    tracker.submitFrame(frameAt(1200, {objectAt(1, QPointF(90.0, 90.0))}), 1200);
    const QPointF position = positionOf(tracker.buildSnapshot(1200), 1);
    check(qAbs(position.x() - 0.1) < 0.001, "a single outlier frame must not move the object at all");
}

/// 수신이 끊겼다 돌아와도 지도 배율은 유지되어야 한다 (화면 전체가 튀는 것 방지).
void checkStreamRestartKeepsWorldBounds() {
    RiskObjectTracker tracker(checkConfig());
    for (int step = 0; step < 3; ++step) {
        const qint64 timeMsec = 1000 + step * 100;
        tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(0.0, 0.0)), objectAt(2, QPointF(40.0, 40.0)),
                                               objectAt(3, QPointF(20.0, 20.0))}),
                            timeMsec);
        tracker.buildSnapshot(timeMsec);
    }
    const double beforeX = positionOf(tracker.buildSnapshot(1200), 3).x();

    // 도착 간격 5초 초과 -> 스트림 재시작으로 간주되어 내부 상태가 초기화된다.
    // 재시작 후에는 관측 분포가 좁아, 경계를 다시 추정하면 같은 좌표가 다른 위치로 간다
    const qint64 restartMsec = 1200 + 6000;
    tracker.submitFrame(frameAt(restartMsec, {objectAt(1, QPointF(0.0, 0.0)), objectAt(3, QPointF(20.0, 20.0))}),
                        restartMsec);
    const double afterX = positionOf(tracker.buildSnapshot(restartMsec), 3).x();

    check(qAbs(beforeX - 0.5) < 0.05, "the probe must sit mid-map before the restart");
    check(qAbs(afterX - beforeX) < 0.001, "a stream restart must not rescale the map");
}
/// RiskFrame.ts가 뒤로 가도 프레임을 버리지 않아야 한다 (멈췄다가 튀는 현상 방지).
/// control-server는 윈도우에 모인 채널 관측 중 가장 오래된 ts를 싣기 때문에, 채널별 CCTV
/// 시계 차이만큼 ts가 역행할 수 있다.
void checkBackwardTimestampStillUpdates() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(0.0, 0.0, 100.0, 100.0);

    RiskObjectTracker tracker(config);
    // 중앙값 필터가 채워지도록 같은 좌표를 세 번 넣고 시작한다
    for (int step = 0; step < 3; ++step) {
        const qint64 timeMsec = 1000 + step * 100;
        tracker.submitFrame(frameAt(5000 + step, {objectAt(1, QPointF(10.0, 10.0))}), timeMsec);
        tracker.buildSnapshot(timeMsec);
    }

    // ts가 300ms 역행한 프레임들: 다른 채널 시계로 넘어간 상황
    qint64 timeMsec = 1300;
    for (int step = 0; step < 3; ++step) {
        timeMsec += 100;
        tracker.submitFrame(frameAt(4700 + step, {objectAt(1, QPointF(14.0, 10.0))}), timeMsec);
        tracker.buildSnapshot(timeMsec);
    }

    const double movedX = positionOf(tracker.buildSnapshot(timeMsec), 1).x();
    check(movedX > 0.11, "frames with a backward source timestamp must still be rendered");
}
/// 채널 경계 위에서 좌표가 흔들려도 채널이 왕복하면 안 된다 (위험 테두리/신고 채널 깜빡임 방지).
void checkChannelDoesNotFlapAtBoundary() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-5.0, -5.0, 10.0, 10.0);  // 실제 담당 구역과 같은 10x10m

    RiskObjectTracker tracker(config);
    qint64 timeMsec = 1000;
    tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(1.5, -2.5))}), timeMsec);
    const DigitalTwinSnapshot first = tracker.buildSnapshot(timeMsec);
    const int startChannel = first.objects.isEmpty() ? -1 : first.objects.constFirst().channelIndex;
    check(startChannel == 2, "below the diagonal must start on CH03");

    // 대각선 경계(|x| = |y|, 여기서는 x = 2.5) 위에서 +-0.1m로 흔들리는 좌표
    int changes = 0;
    for (int step = 0; step < 20; ++step) {
        timeMsec += 100;
        const double x = (step % 2 == 0) ? 2.6 : 2.4;
        tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(x, -2.5))}), timeMsec);
        const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(timeMsec);
        if (!snapshot.objects.isEmpty() && snapshot.objects.constFirst().channelIndex != startChannel) {
            ++changes;
        }
    }
    check(changes == 0, "jitter around the channel boundary must not change the channel");

    // 경계를 확실히 넘어가면 채널은 바뀌어야 한다
    for (int step = 0; step < 10; ++step) {
        timeMsec += 100;
        tracker.submitFrame(frameAt(timeMsec, {objectAt(1, QPointF(4.0, -2.5))}), timeMsec);
        tracker.buildSnapshot(timeMsec);
    }
    const DigitalTwinSnapshot moved = tracker.buildSnapshot(timeMsec);
    check(!moved.objects.isEmpty() && moved.objects.constFirst().channelIndex != startChannel,
          "crossing the boundary for real must change the channel");
}
/// 중심에서 X자로 자른 네 구역과 채널 번호의 대응. 위 CH01, 왼쪽 CH02, 아래 CH03, 오른쪽 CH04.
void checkWedgeChannelMapping() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-5.0, -5.0, 10.0, 10.0);
    config.world.invertY = true;  // 월드 +Y는 위쪽이다

    RiskObjectTracker tracker(config);
    // 각 구역 한가운데. 대각선 위가 아니라 축 위에 두어야 구역이 명확하다
    tracker.submitFrame(frameAt(1000, {objectAt(1, QPointF(0.0, 3.0)), objectAt(2, QPointF(3.0, 0.0)),
                                       objectAt(3, QPointF(0.0, -3.0)), objectAt(4, QPointF(-3.0, 0.0))}),
                        1000);

    const DigitalTwinSnapshot snapshot = tracker.buildSnapshot(1000);
    check(snapshot.objects.size() == 4, "all four wedge objects must be present");
    for (const DigitalTwinObject& object : snapshot.objects) {
        if (object.objectId == QStringLiteral("G-1")) {
            check(object.channelIndex == 0, "the north wedge must be CH01");
        } else if (object.objectId == QStringLiteral("G-2")) {
            check(object.channelIndex == 3, "the east wedge must be CH04");
        } else if (object.objectId == QStringLiteral("G-3")) {
            check(object.channelIndex == 2, "the south wedge must be CH03");
        } else if (object.objectId == QStringLiteral("G-4")) {
            check(object.channelIndex == 1, "the west wedge must be CH02");
        }
    }
}
/// 같은 쌍이 프레임마다 사라졌다 나타나도 위험 파동이 쏟아지면 안 된다.
void checkRiskPulsesAreRateLimited() {
    DigitalTwinRuntimeConfig config = checkConfig();
    config.world.fixedBoundsEnabled = true;
    config.world.bounds = QRectF(-5.0, -5.0, 10.0, 10.0);
    // 객체 보존을 끊어야 쌍이 실제로 스냅샷에서 사라진다(기본값이면 280ms 동안 남아 깜빡이지 않는다)
    config.missingGraceMsec = 0;
    config.fadeOutMsec = 0;

    RiskObjectTracker tracker(config);
    int pulseCount = 0;
    qint64 timeMsec = 1000;
    for (int step = 0; step < 40; ++step) {
        timeMsec += 100;

        // 서로를 최근접으로 가리키는 위험 쌍. 한 프레임씩 건너뛰며 사라졌다 나타난다
        QVector<RiskObjectData> objects;
        if (step % 2 == 0) {
            RiskObjectData first = objectAt(1, QPointF(-1.0, 0.0));
            first.riskLevel = DigitalTwinRiskLevel::Danger;
            first.nearestId = 2;
            first.distance = 2.0;
            RiskObjectData second = objectAt(2, QPointF(1.0, 0.0));
            second.riskLevel = DigitalTwinRiskLevel::Danger;
            second.nearestId = 1;
            second.distance = 2.0;
            objects = {first, second};
        }

        tracker.submitFrame(frameAt(timeMsec, objects), timeMsec);
        tracker.buildSnapshot(timeMsec);
        pulseCount += static_cast<int>(tracker.takeRiskEvents().size());
    }

    // 4초 동안 1.5초 주기라면 3회 안팎이어야 한다. 예전 로직은 재등장마다 울려 20회 가까이 나왔다
    check(pulseCount > 0, "a dangerous pair must pulse at least once");
    check(pulseCount <= 6, "a flapping pair must not flood the map with pulses");
}
}  // namespace

int main() {
    checkNarrowObservationStaysCentered();
    checkOutOfRangePositionExpandsBounds();
    checkSingleFrameTeleportIsRateLimited();
    checkSustainedMovementCatchesUp();
    checkIsolatedOutlierFrameIsRejected();
    checkStreamRestartKeepsWorldBounds();
    checkBackwardTimestampStillUpdates();
    checkChannelDoesNotFlapAtBoundary();
    checkWedgeChannelMapping();
    checkRiskPulsesAreRateLimited();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskObjectTracker bounds checks passed\n");
    return 0;
}
