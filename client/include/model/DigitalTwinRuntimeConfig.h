#pragma once

#include <QRectF>
#include <QVector>
#include <QtGlobal>
#include <limits>

/// 도면 격자가 4열 x 2행이므로 구역은 최대 8개다.
/// 더 늘리려면 qml/ParkingPlan.js의 ZONE_COLS/ZONE_ROWS와 홀 가로세로비부터 바꿔야 한다
/// (셀이 정사각형이 아니면 객체의 가로·세로 배율이 어긋난다)
inline constexpr int digitalTwinMaximumZoneCount = 8;

struct DigitalTwinWorldConfig {
    QRectF bounds = QRectF(0.0, 0.0, 100.0, 100.0);
    /// 물리 CCTV 구역별 월드 상자. 원소가 비어 있으면 bounds를 구역 수만큼 x로 갈라 쓴다
    QVector<QRectF> zones;
    bool fixedBoundsEnabled = false;
    bool invertY = true;
    int automaticBoundsWarmupMsec = 500;
    qsizetype automaticBoundsMinimumSamples = 12;
    qsizetype automaticBoundsMaximumSamples = 512;
    double automaticBoundsPaddingRatio = 0.08;
    double automaticBoundsOutlierFraction = 0.05;

    /// 활성 구역 수. 설정 로더가 video.areas 개수에 맞춰 zones 길이를 채운다
    int zoneCount() const { return qMax(1, static_cast<int>(zones.size())); }

    /**
     * @brief            물리 CCTV 구역 하나가 차지하는 월드 상자를 돌려줍니다.
     * @param zoneIndex  0부터 zoneCount() - 1까지
     *
     * @details bounds를 균등하게 가르는 방식은 구역들이 서로 붙어 있고 폭도 같아야 하므로,
     *          도면에서 멀리 떨어진 구역들(예: 100m 간격의 15m짜리 구역)을 표현할 수 없다.
     *          그 배치에서는 상자를 아무리 조정해도 한쪽이 지도 끝에 뭉친다.
     *          상자가 설정되어 있으면 그것을 쓰고, 없을 때만 균등 가르기로 돌아간다.
     */
    QRectF zoneBounds(int zoneIndex) const {
        const int boundedIndex = qBound(0, zoneIndex, zoneCount() - 1);
        const QRectF zone = zones.value(boundedIndex);
        if (zone.width() > 0.0 && zone.height() > 0.0) {
            return zone;
        }

        const double columnWidth = bounds.width() / zoneCount();
        return QRectF(bounds.left() + boundedIndex * columnWidth, bounds.top(), columnWidth, bounds.height());
    }

    /**
     * @brief          zoneId를 못 받았을 때 x 좌표로 구역을 고릅니다.
     * @param worldX   객체의 월드 좌표 x
     * @return         상자가 x를 품는 구역, 없으면 중심이 가장 가까운 구역
     *
     * @details bounds의 중심으로 가르면 구역들이 도면 한쪽에 몰려 있을 때 경계가 엉뚱한 곳에
     *          생긴다. 실제 구역 상자들과 비교해야 세 개 이상일 때도 맞는다.
     */
    int zoneIndexForWorldX(double worldX) const {
        int nearestIndex = 0;
        double nearestDistance = std::numeric_limits<double>::max();
        for (int index = 0; index < zoneCount(); ++index) {
            const QRectF zone = zoneBounds(index);
            if (worldX >= zone.left() && worldX <= zone.right()) {
                return index;
            }

            const double distance = qAbs(worldX - zone.center().x());
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestIndex = index;
            }
        }
        return nearestIndex;
    }
};

/// 탑뷰 맵에 그리는 객체 아이콘의 한 변 길이(scene 단위). 맵 자체는 1000x520이다
struct DigitalTwinIconConfig {
    int vehiclePixels = 92;
    int pedestrianPixels = 62;
};

struct DigitalTwinRuntimeConfig {
    int renderIntervalMsec = 33;
    int snapshotPublishIntervalMsec = 50;
    int frameExpiryMsec = 5000;
    int frameExpiryPollMsec = 1000;
    // Local UI interpolation only; this is not a video/source timestamp delay.
    qint64 positionTransitionMsec = 100;
    qint64 fadeInMsec = 120;
    qint64 missingGraceMsec = 200;
    qint64 fadeOutMsec = 180;
    qsizetype maximumHistorySize = 2;
    int diagnosticsIntervalMsec = 1000;
    /// 탑뷰 수신 요약과 경계/이상치 로그
    bool debugLogging = false;
    /// 객체별 프레임 상세 좌표 로그 (debugLogging이 켜져 있어야 의미가 있다)
    bool debugDetail = false;
    /// 객체별 상세 로그를 gid마다 이 주기로 제한한다. 필터가 개입한 프레임은 주기와 무관하게 남는다
    int debugDetailIntervalMsec = 1000;
    DigitalTwinWorldConfig world;
    DigitalTwinIconConfig icons;
};
