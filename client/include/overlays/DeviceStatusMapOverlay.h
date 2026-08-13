#pragma once

#include <QPixmap>
#include <QRectF>
#include <QVector>
#include <array>

#include "model/DeviceStatus.h"
#include "model/DigitalTwinMapDisplaySettings.h"

class QGraphicsPixmapItem;
class QGraphicsScene;

class DeviceStatusMapOverlay final {
public:
    void initialize(QGraphicsScene* scene, const std::array<QRectF, 2>& zoneRects,
                    const std::array<QRectF, 2>& zoneStatusSlots);
    void setSignalAvailable(bool available);
    void setChannelStatuses(const QVector<DeviceChannelStatus>& statuses);
    void setDisplaySettings(const DigitalTwinMapDisplaySettings& settings);

private:
    /// 채널별 수신 상태. 화면에는 구역 단위로 집약해서 표시한다
    struct ChannelStatusRecord {
        DeviceChannelStatus status;
        bool hasStatus = false;
        bool receivedInCurrentSession = false;
    };

    /// 구역 하나를 대표하는 장치 상태 아이콘 한 쌍
    struct ZoneVisualItems {
        QGraphicsPixmapItem* led = nullptr;
        QGraphicsPixmapItem* sensor = nullptr;
    };

    void loadPixmaps();
    void updateAllZones();
    void updateZone(int zoneIndex);
    bool hasValidSignal(const ChannelStatusRecord& record) const;
    const QPixmap& ledPixmap(const DeviceOutputState& outputs) const;
    QPixmap loadScaledPixmap(const QString& resourcePath, int size) const;

    std::array<ChannelStatusRecord, 8> channels_;
    std::array<ZoneVisualItems, 2> zones_;
    QPixmap ledOffPixmap_;
    QPixmap ledSafePixmap_;
    QPixmap ledWarningPixmap_;
    QPixmap ledDangerPixmap_;
    QPixmap sensorOffPixmap_;
    QPixmap sensorSafePixmap_;
    QPixmap sensorActivePixmap_;
    QPixmap cctvPixmap_;
    std::array<QGraphicsPixmapItem*, 2> cctvItems_ = {nullptr, nullptr};
    DigitalTwinMapDisplaySettings displaySettings_;
    bool signalAvailable_ = false;
};
