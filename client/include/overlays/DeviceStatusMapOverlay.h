#pragma once

#include <QPixmap>
#include <QVector>
#include <array>

#include "model/DeviceStatus.h"
#include "model/DigitalTwinMapDisplaySettings.h"

class QGraphicsPixmapItem;
class QGraphicsScene;
class QGraphicsSimpleTextItem;

class DeviceStatusMapOverlay final {
public:
    void initialize(QGraphicsScene* scene);
    void setSignalAvailable(bool available);
    void setChannelStatuses(const QVector<DeviceChannelStatus>& statuses);
    void setDisplaySettings(const DigitalTwinMapDisplaySettings& settings);

private:
    struct ChannelVisualItems {
        QGraphicsPixmapItem* led = nullptr;
        QGraphicsPixmapItem* sensor = nullptr;
        QGraphicsSimpleTextItem* channelLabel = nullptr;
        DeviceChannelStatus status;
        bool hasStatus = false;
        bool receivedInCurrentSession = false;
    };

    void loadPixmaps();
    void updateAllChannels();
    void updateChannel(int channelIndex);
    bool hasValidSignal(const ChannelVisualItems& items) const;
    const QPixmap& ledPixmap(const DeviceOutputState& outputs) const;
    QPixmap loadScaledPixmap(const QString& resourcePath, int size) const;

    std::array<ChannelVisualItems, 4> channels_;
    QPixmap ledOffPixmap_;
    QPixmap ledSafePixmap_;
    QPixmap ledWarningPixmap_;
    QPixmap ledDangerPixmap_;
    QPixmap sensorOffPixmap_;
    QPixmap sensorSafePixmap_;
    QPixmap sensorActivePixmap_;
    QPixmap cctvPixmap_;
    QGraphicsPixmapItem* cctvItem_ = nullptr;
    DigitalTwinMapDisplaySettings displaySettings_;
    bool signalAvailable_ = false;
};
