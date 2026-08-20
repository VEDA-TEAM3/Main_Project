#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QWidget>
#include <memory>

#include "model/DeviceStatus.h"
#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/DigitalTwinRuntimeConfig.h"
#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"
#include "ui/DigitalTwinZoneIndex.h"

class DigitalTwinObjectStyleProvider;
class RiskObjectTracker;
class QQuickItem;
class QQuickWidget;

/**
 * @brief 디지털 트윈 2D 맵.
 *
 * @details 도면과 모든 표시는 qml/DigitalTwinMap.qml이 그립니다. 이 클래스는 데이터만 맡습니다 —
 *          시뮬레이션 worker, 실시간 위험 프레임 추적, 중앙 이벤트, 장치 상태를 모아 QML 속성에
 *          밀어 넣고, QML이 올려 보내는 구역 클릭을 다시 신호로 냅니다.
 *
 *          QML로 넘기는 목록에는 **QVariantMap을 담지 않습니다.** 평평한 배열을 QVariant로 감싸
 *          넘기고 QML이 자리 순서로 읽습니다(CLAUDE.md의 DeviceStatusPanel 항목과 같은 이유).
 */
class DigitalTwinMapWidget : public QWidget {
    Q_OBJECT

public:
    explicit DigitalTwinMapWidget(QWidget* parent = nullptr);
    ~DigitalTwinMapWidget() override;

    void applyDisplaySettings(const DigitalTwinMapDisplaySettings& settings);
    void configureLiveTracking(const DigitalTwinRuntimeConfig& config);

public slots:
    void applyRiskFrame(RiskFrameData frame);
    void applyCentralEvent(CentralEventData event);
    void applyDeviceChannelStatuses(QVector<DeviceChannelStatus> statuses);
    void setDeviceSignalAvailable(bool available);

signals:
    void simulationSnapshotUpdated(DigitalTwinSnapshot snapshot);
    void channelRiskLevelsChanged(QVector<DigitalTwinRiskLevel> riskLevels);
    /** @brief 지도에서 CCTV가 있는 구역을 클릭했을 때 그 구역 인덱스를 알립니다. */
    void zoneSelected(int zoneIndex);

private slots:
    /** @brief QML 구역 클릭을 받아 다시 알립니다. QML 루트의 신호는 문자열로만 연결됩니다. */
    void handleZoneClicked(int zoneIndex);

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// 객체 하나의 표시 상태. 위치는 전부 도면(plan) 좌표다
    struct ObjectVisual {
        DigitalTwinObject object;
        QVector<QPointF> recentPositions;
        /// 화면상 이동 벡터의 지수이동평균. 아이콘 방향을 여기서 낸다
        QPointF smoothedPlanVelocity;
        QPointF planPosition;
        QPointF previousPlanPosition;
        double visibleRotationDegrees = 0.0;
        bool hasPreviousPlanPosition = false;
        bool hasRotation = false;
    };

    /// 채널 하나의 장치 수신 상태. 화면에는 단계 번호로만 나간다
    struct DeviceRecord {
        DeviceChannelStatus status;
        bool hasStatus = false;
        bool receivedInCurrentSession = false;
    };

    void ensureMapReady();
    /// 현재 구역 수가 감당하는 전체 채널 수
    int liveChannelCount() const { return zoneCount_ * digitalTwinChannelsPerZone; }
    void applyObjectUpdates(const DigitalTwinSnapshot& snapshot);
    void publishChannelRiskLevels(const DigitalTwinSnapshot& snapshot);
    void publishObjects();
    void publishTrails(bool force);
    void publishDeviceStates();
    void publishDisplaySettings();
    void setMapProperty(const char* name, const QVariant& value);
    void rebuildLiveSnapshot();
    int activeSeverityForChannel(int channelIndex) const;
    QVector<DigitalTwinRiskLevel> channelRiskLevels(const DigitalTwinSnapshot& snapshot) const;
    bool hasActiveCentralDanger() const;
    void setDangerActive(bool active);
    void updateObjectVisual(ObjectVisual* visual);
    bool removeMissingVisuals(const QVector<DigitalTwinObject>& objects);
    void rebuildVisualIndexes();
    void refreshObjectAreas();
    QPointF planPointForObject(const QPointF& position, int channelIndex) const;
    QVector<QPointF> visibleTrail(const QVector<QPointF>& positions) const;

    QQuickWidget* mapView_ = nullptr;
    QQuickItem* mapRoot_ = nullptr;
    DigitalTwinMapDisplaySettings displaySettings_;
    DigitalTwinRuntimeConfig liveConfig_;
    std::shared_ptr<DigitalTwinObjectStyleProvider> objectStyleProvider_;
    std::unique_ptr<RiskObjectTracker> riskObjectTracker_;
    QVector<ObjectVisual> visuals_;
    QHash<QString, qsizetype> visualIndexes_;
    QHash<QString, qint64> latestCentralEventSourceTimes_;
    QHash<QString, CentralEventData> activeCentralEvents_;
    QVector<DeviceRecord> deviceChannels_;
    QElapsedTimer liveClock_;
    QTimer liveFrameRenderTimer_;
    QVector<DigitalTwinRiskLevel> publishedChannelRiskLevels_;
    QVariantList publishedObjectPayload_;
    QVariantList publishedTrailPayload_;
    bool hasPublishedObjectPayload_ = false;
    bool hasPublishedTrailPayload_ = false;
    /// 구역별 객체 표시 영역. QML의 ParkingPlan.js가 원본이라 거기서 읽어 온다
    QVector<QRectF> objectAreaRects_;
    qint64 lastLiveSnapshotPublishMsec_ = 0;
    qint64 lastTrailPublishMsec_ = 0;
    /// 이동 경로에는 마지막으로 점을 찍은 수신 샘플 번호. 렌더 보간 프레임을 걸러 내는 기준이다
    qint64 lastTrailSampleSequence_ = -1;
    /// 지금 처리 중인 스냅샷이 새 수신 샘플인지. applyObjectUpdates가 한 번 정하고 함께 읽는다
    bool trailSampleFrame_ = true;
    /// 활성 CCTV 구역 수. 지도를 세우기 전에 configureLiveTracking이 실제 값으로 바꾼다
    int zoneCount_ = 2;
    bool mapReady_ = false;
    bool liveMode_ = false;
    bool deviceSignalAvailable_ = false;
    bool dangerActive_ = false;
};
