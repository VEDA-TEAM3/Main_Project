#pragma once

#include <QElapsedTimer>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
#include <QRectF>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <array>
#include <memory>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/DigitalTwinRuntimeConfig.h"
#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"
#include "overlays/ChannelRiskOverlay.h"
#include "overlays/DangerBorderOverlay.h"
#include "overlays/DeviceStatusMapOverlay.h"
#include "ui/DigitalTwinMapSceneBuilder.h"
#include "ui/DigitalTwinZoneIndex.h"

class DigitalTwinSimulationWorker;
class DigitalTwinObjectStyleProvider;
class RiskObjectTracker;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsSimpleTextItem;
class QMouseEvent;
class QPainterPath;
class QResizeEvent;
class QShowEvent;

class DigitalTwinMapWidget : public QGraphicsView {
    Q_OBJECT

public:
    explicit DigitalTwinMapWidget(QWidget* parent = nullptr);
    ~DigitalTwinMapWidget() override;

    void startDemo();
    void stopDemo();
    void applyDisplaySettings(const DigitalTwinMapDisplaySettings& settings);
    void configureLiveTracking(const DigitalTwinRuntimeConfig& config);

public slots:
    void applyRiskFrame(RiskFrameData frame);
    void applyCentralEvent(CentralEventData event);
    void applyDeviceChannelStatuses(QVector<DeviceChannelStatus> statuses);
    void setDeviceSignalAvailable(bool available);

signals:
    void liveRiskStreamActivated();
    void simulationSnapshotUpdated(DigitalTwinSnapshot snapshot);
    void channelRiskLevelsChanged(QVector<DigitalTwinRiskLevel> riskLevels);
    /** @brief 지도에서 CCTV가 있는 구역을 클릭했을 때 그 구역 인덱스를 알립니다. */
    void zoneSelected(int zoneIndex);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct DemoVisualItem {
        DigitalTwinObject object;
        QGraphicsPixmapItem* marker = nullptr;
        QGraphicsSimpleTextItem* label = nullptr;
        QGraphicsPathItem* trail = nullptr;
        QVector<QPointF> recentPositions;
        DigitalTwinRiskLevel visibleRiskLevel = DigitalTwinRiskLevel::Normal;
    };

    void ensureSceneReady();
    void setupScene();
    void setupSimulationWorker();
    /// 현재 구역 수가 감당하는 전체 채널 수
    int liveChannelCount() const { return zoneCount_ * digitalTwinChannelsPerZone; }
    void applySimulationSnapshot(const DigitalTwinSnapshot& snapshot);
    void applyObjectUpdates(const QVector<DigitalTwinObject>& objects);
    void publishChannelRiskLevels(const DigitalTwinSnapshot& snapshot);
    int zoneIndexAtScenePosition(const QPointF& scenePosition) const;
    void rebuildLiveSnapshot();
    int activeSeverityForChannel(int channelIndex) const;
    QVector<DigitalTwinRiskLevel> channelRiskLevels(const DigitalTwinSnapshot& snapshot) const;
    bool hasActiveCentralDanger() const;
    void expireStaleLiveFrames();
    void createVisualItem(const DigitalTwinObject& object);
    void updateVisualItem(DemoVisualItem* visualItem);
    void updateMarkerPixmap(DemoVisualItem* visualItem);
    void removeMissingVisualItems(const QVector<DigitalTwinObject>& objects);
    void removeVisualItemAt(qsizetype visualIndex);
    void rebuildVisualItemIndexes();
    void updateObjectAreaRect();
    QPointF scenePointForObject(const QPointF& position, int channelIndex) const;
    QPainterPath createTrailPath(const QVector<QPointF>& positions) const;
    void fitMapInView();

    QGraphicsScene scene_;
    QThread simulationThread_;
    // ChannelRiskOverlay는 QTimer와 unique_ptr을 들고 있어 복사도 이동도 되지 않는다
    QVector<std::shared_ptr<ChannelRiskOverlay>> channelRiskOverlays_;
    DeviceStatusMapOverlay deviceStatusMapOverlay_;
    DigitalTwinMapDisplaySettings displaySettings_;
    DigitalTwinRuntimeConfig liveConfig_;
    DangerBorderOverlay dangerBorderOverlay_;
    std::shared_ptr<DigitalTwinMapSceneBuilder> sceneBuilder_;
    std::shared_ptr<DigitalTwinObjectStyleProvider> objectStyleProvider_;
    std::shared_ptr<DigitalTwinSimulationWorker> simulationWorker_;
    std::unique_ptr<RiskObjectTracker> riskObjectTracker_;
    QVector<DemoVisualItem> demoItems_;
    QHash<QString, qsizetype> visualItemIndexes_;
    QHash<QString, qint64> latestCentralEventSourceTimes_;
    QHash<QString, CentralEventData> activeCentralEvents_;
    QElapsedTimer liveClock_;
    QTimer liveFrameExpiryTimer_;
    QTimer liveFrameRenderTimer_;
    QVector<DigitalTwinRiskLevel> publishedChannelRiskLevels_;
    qint64 lastLiveSnapshotPublishMsec_ = 0;
    DigitalTwinMapSceneLayout mapLayout_;
    QVector<QRectF> objectAreaRects_;
    /// 활성 CCTV 구역 수. scene을 세우기 전에 configureLiveTracking이 실제 값으로 바꾼다
    int zoneCount_ = 2;
    bool sceneReady_ = false;
    bool liveMode_ = false;
};
