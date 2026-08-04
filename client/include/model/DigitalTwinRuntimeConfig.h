#pragma once

#include <QRectF>
#include <QtGlobal>

struct DigitalTwinWorldConfig {
    QRectF bounds = QRectF(0.0, 0.0, 100.0, 100.0);
    bool fixedBoundsEnabled = false;
    bool invertY = true;
    int automaticBoundsWarmupMsec = 500;
    qsizetype automaticBoundsMinimumSamples = 12;
    qsizetype automaticBoundsMaximumSamples = 512;
    double automaticBoundsPaddingRatio = 0.08;
    double automaticBoundsOutlierFraction = 0.05;
};

struct DigitalTwinRuntimeConfig {
    int renderIntervalMsec = 33;
    int snapshotPublishIntervalMsec = 50;
    int frameExpiryMsec = 5000;
    int frameExpiryPollMsec = 1000;
    // Local UI interpolation only; this is not a video/source timestamp delay.
    qint64 positionTransitionMsec = 100;
    qint64 fadeInMsec = 120;
    qint64 missingGraceMsec = 100;
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
};
