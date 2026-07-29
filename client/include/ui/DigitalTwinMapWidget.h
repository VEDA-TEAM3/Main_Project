#pragma once

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
#include <QRectF>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"
#include "overlays/DeviceStatusMapOverlay.h"
#include "overlays/OverlayManager.h"

class DigitalTwinSimulationWorker;
class DigitalTwinMapSceneBuilder;
class DigitalTwinObjectStyleProvider;
class RiskObjectTracker;
class DangerAlertOverlay;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsSimpleTextItem;
class QPainterPath;
class QResizeEvent;

class DigitalTwinMapWidget : public QGraphicsView {
    Q_OBJECT

public:
    explicit DigitalTwinMapWidget(QWidget* parent = nullptr);
    ~DigitalTwinMapWidget() override;

    void startDemo();
    void stopDemo();
    void applyDisplaySettings(const DigitalTwinMapDisplaySettings& settings);

public slots:
    void applyRiskFrame(RiskFrameData frame);
    void applyCentralEvent(CentralEventData event);
    void applyDeviceChannelStatuses(QVector<DeviceChannelStatus> statuses);
    void setDeviceSignalAvailable(bool available);

signals:
    void liveRiskStreamActivated();
    void simulationSnapshotUpdated(DigitalTwinSnapshot snapshot);
    void channelRiskLevelsChanged(QVector<DigitalTwinRiskLevel> riskLevels);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct DemoVisualItem {
        DigitalTwinObject object;
        QGraphicsPixmapItem* marker = nullptr;
        QGraphicsSimpleTextItem* label = nullptr;
        QGraphicsPathItem* trail = nullptr;
        QVector<QPointF> recentPositions;
        DigitalTwinRiskLevel visibleRiskLevel = DigitalTwinRiskLevel::Normal;
    };

    void setupScene();
    void setupSimulationWorker();
    void applySimulationSnapshot(const DigitalTwinSnapshot& snapshot);
    void applyObjectUpdates(const QVector<DigitalTwinObject>& objects);
    void showRiskPulse(const DigitalTwinRiskEvent& event);
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
    QPointF scenePointFromNormalized(const QPointF& normalizedPosition) const;
    QPainterPath createTrailPath(const QVector<QPointF>& positions) const;
    void fitMapInView();

    QGraphicsScene scene_;
    QThread simulationThread_;
    OverlayManager overlayManager_;
    DeviceStatusMapOverlay deviceStatusMapOverlay_;
    DigitalTwinMapDisplaySettings displaySettings_;
    DangerAlertOverlay* dangerAlertOverlay_ = nullptr;
    std::shared_ptr<DigitalTwinMapSceneBuilder> sceneBuilder_;
    std::shared_ptr<DigitalTwinObjectStyleProvider> objectStyleProvider_;
    std::shared_ptr<DigitalTwinSimulationWorker> simulationWorker_;
    std::unique_ptr<RiskObjectTracker> riskObjectTracker_;
    QVector<DemoVisualItem> demoItems_;
    QHash<QString, qsizetype> visualItemIndexes_;
    QHash<QString, qint64> latestCentralEventSourceTimes_;
    QHash<QString, CentralEventData> activeCentralEvents_;
    QTimer liveFrameExpiryTimer_;
    QTimer liveFrameRenderTimer_;
    qint64 lastLiveSnapshotPublishMsec_ = 0;
    QRectF mapRect_;
    bool liveMode_ = false;
};
