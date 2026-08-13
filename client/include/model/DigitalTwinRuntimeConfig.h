#pragma once

#include <QRectF>
#include <QtGlobal>
#include <array>

struct DigitalTwinWorldConfig {
    QRectF bounds = QRectF(0.0, 0.0, 100.0, 100.0);
    /// 물리 CCTV 구역별 월드 상자. 비어 있으면 bounds를 x로 반 갈라 쓴다
    std::array<QRectF, 2> zones;
    bool fixedBoundsEnabled = false;
    bool invertY = true;
    int automaticBoundsWarmupMsec = 500;
    qsizetype automaticBoundsMinimumSamples = 12;
    qsizetype automaticBoundsMaximumSamples = 512;
    double automaticBoundsPaddingRatio = 0.08;
    double automaticBoundsOutlierFraction = 0.05;

    /**
     * @brief            물리 CCTV 구역 하나가 차지하는 월드 상자를 돌려줍니다.
     * @param zoneIndex  0 또는 1
     *
     * @details bounds를 x로 반 가르는 방식은 두 창이 서로 붙어 있고 폭도 같아야 하므로,
     *          도면에서 멀리 떨어진 두 구역(예: 100m 간격의 15m짜리 구역 두 개)을 표현할 수
     *          없다. 그 배치에서는 상자를 아무리 조정해도 한쪽이 지도 끝에 뭉친다.
     *          zones가 설정되어 있으면 그것을 쓰고, 없을 때만 기존 반 가르기로 돌아간다.
     */
    QRectF zoneBounds(int zoneIndex) const {
        const int boundedIndex = qBound(0, zoneIndex, static_cast<int>(zones.size()) - 1);
        const QRectF& zone = zones[boundedIndex];
        if (zone.width() > 0.0 && zone.height() > 0.0) {
            return zone;
        }

        const double halfWidth = bounds.width() * 0.5;
        return QRectF(bounds.left() + boundedIndex * halfWidth, bounds.top(), halfWidth, bounds.height());
    }

    /**
     * @brief  zoneId를 못 받았을 때 x 좌표로 구역을 가르는 기준값입니다.
     * @return 두 구역 상자 사이의 중간 x
     *
     * @details bounds의 중심을 쓰면 두 구역이 도면 한쪽에 몰려 있을 때 경계가 엉뚱한 곳에 생긴다.
     *          zones가 없으면 두 상자가 bounds를 반 가른 결과라 이 값이 그대로 중심이 된다.
     */
    double zoneSplitX() const { return (zoneBounds(0).right() + zoneBounds(1).left()) * 0.5; }
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
