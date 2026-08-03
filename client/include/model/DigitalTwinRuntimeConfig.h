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
    qint64 renderDelayMsec = 100;
    bool syncWithVideo = true;
    qint64 videoSyncCorrectionMsec = 0;
    qint64 maximumVideoClockSkewMsec = 3000;
    qint64 videoTimestampTimeoutMsec = 500;
    qint64 channelTimestampOutlierMsec = 500;
    qint64 fadeInMsec = 120;
    qint64 missingGraceMsec = 350;
    qint64 fadeOutMsec = 180;
    qsizetype maximumHistorySize = 16;
    int diagnosticsIntervalMsec = 1000;
    DigitalTwinWorldConfig world;
};
