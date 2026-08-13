#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "model/DigitalTwinTypes.h"
#include "model/EventLogGenerator.h"
#include "model/MqttRealtimeData.h"

class DeviceStatusPanel;
class DeviceStatusService;
class EventLogPanel;
class ObjectListPanel;

class DashboardPanelCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit DashboardPanelCoordinator(DeviceStatusPanel* deviceStatusPanel, EventLogPanel* eventLogPanel,
                                       ObjectListPanel* objectListPanel, QObject* parent = nullptr);

    void bindDeviceStatusService(DeviceStatusService* service);

public slots:
    void consumeDigitalTwinSnapshot(DigitalTwinSnapshot snapshot);
    void consumeCentralEvent(CentralEventData event);
    void resetEventLogForLiveInput();

private:
    void flushObjectList();

    DeviceStatusPanel* deviceStatusPanel_ = nullptr;
    EventLogPanel* eventLogPanel_ = nullptr;
    ObjectListPanel* objectListPanel_ = nullptr;
    EventLogGenerator eventLogGenerator_;
    QHash<QString, qint64> latestCentralEventTimestamps_;
    QVector<DigitalTwinObject> pendingObjects_;
    QTimer objectListFlushTimer_;
    bool hasPendingObjects_ = false;
};
