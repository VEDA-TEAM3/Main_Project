#include "ui/panels/DashboardPanelCoordinator.h"

#include <QDateTime>
#include <utility>

#include "network/DeviceStatusService.h"
#include "ui/panels/DeviceStatusPanel.h"
#include "ui/panels/EventLogPanel.h"
#include "ui/panels/ObjectListPanel.h"

namespace {
constexpr int objectListFlushIntervalMsec = 200;
}  // namespace

/**
 * @brief                    패널 데이터 갱신 경로를 한곳에 모으는 coordinator를 생성합니다.
 * @param deviceStatusPanel  장비 출력 상태 패널
 * @param eventLogPanel      이벤트 로그 패널
 * @param objectListPanel    실시간 객체 목록 패널
 * @param parent             Qt 객체 소유권을 연결할 부모 객체
 */
DashboardPanelCoordinator::DashboardPanelCoordinator(DeviceStatusPanel* deviceStatusPanel, EventLogPanel* eventLogPanel,
                                                     ObjectListPanel* objectListPanel, QObject* parent)
    : QObject(parent),
      deviceStatusPanel_(deviceStatusPanel),
      eventLogPanel_(eventLogPanel),
      objectListPanel_(objectListPanel) {
    objectListFlushTimer_.setInterval(objectListFlushIntervalMsec);
    objectListFlushTimer_.setSingleShot(true);
    objectListFlushTimer_.setTimerType(Qt::CoarseTimer);
    connect(&objectListFlushTimer_, &QTimer::timeout, this, &DashboardPanelCoordinator::flushObjectList);
}

/**
 * @brief          장비 상태 service의 병합된 UI 상태를 장비 패널에 연결합니다.
 * @param service  demo 또는 실제 MQTT gateway를 소유한 service
 */
void DashboardPanelCoordinator::bindDeviceStatusService(DeviceStatusService* service) {
    if (!service || !deviceStatusPanel_) {
        return;
    }

    connect(service, &DeviceStatusService::channelStatusesReceived, deviceStatusPanel_,
            &DeviceStatusPanel::setChannelStatuses, Qt::QueuedConnection);
}

/**
 * @brief          모든 스냅샷의 이벤트 전이를 처리하고 객체 목록은 최신 frame만 보관합니다.
 * @param snapshot 디지털 트윈 worker가 계산한 최신 스냅샷
 */
void DashboardPanelCoordinator::consumeDigitalTwinSnapshot(DigitalTwinSnapshot snapshot) {
    QVector<EventLogEntry> entries = eventLogGenerator_.createEntriesFromSnapshot(snapshot);

    if (eventLogPanel_ && !entries.isEmpty()) {
        eventLogPanel_->prependEntries(std::move(entries));
    }

    pendingObjects_ = std::move(snapshot.objects);
    hasPendingObjects_ = true;

    if (!objectListFlushTimer_.isActive()) {
        objectListFlushTimer_.start();
    }
}

void DashboardPanelCoordinator::consumeCentralEvent(CentralEventData event) {
    if (!eventLogPanel_ || event.channelIndex < 0 || event.channelIndex >= 4 || event.sourceTimestamp <= 0) {
        return;
    }

    const QString identity = event.eventId.isEmpty() ? event.eventType : event.eventId;
    const QString key = QStringLiteral("%1:%2").arg(event.channelIndex).arg(identity);
    if (event.sourceTimestamp <= latestCentralEventTimestamps_.value(key, 0)) {
        return;
    }
    latestCentralEventTimestamps_.insert(key, event.sourceTimestamp);

    if (!event.active) {
        return;
    }

    EventLogEntry entry;
    entry.time = QDateTime::fromMSecsSinceEpoch(event.sourceTimestamp).time();
    entry.area = QStringLiteral("CH-%1").arg(event.channelIndex + 1, 2, 10, QLatin1Char('0'));
    entry.objectText = event.eventType;

    if (event.severity >= 3) {
        entry.riskLevel = EventLogRiskLevel::Danger;
        entry.action = EventLogAction::DangerAlertActivated;
    } else if (event.severity > 0) {
        entry.riskLevel = EventLogRiskLevel::Warning;
        entry.action = EventLogAction::WarningAlertActivated;
    } else {
        entry.riskLevel = EventLogRiskLevel::Normal;
        entry.action = EventLogAction::None;
    }

    eventLogPanel_->prependEntry(entry);
}

/** @brief 데모에서 실시간 MQTT 입력으로 전환될 때 데모 이벤트 이력을 제거합니다. */
void DashboardPanelCoordinator::resetEventLogForLiveInput() {
    eventLogGenerator_.reset();
    latestCentralEventTimestamps_.clear();

    if (eventLogPanel_) {
        eventLogPanel_->clear();
    }
}

/**
 * @brief   누적 대기열 대신 가장 최근 객체 목록만 UI table에 반영합니다.
 */
void DashboardPanelCoordinator::flushObjectList() {
    if (!hasPendingObjects_) {
        return;
    }

    hasPendingObjects_ = false;

    if (objectListPanel_) {
        objectListPanel_->setObjects(std::move(pendingObjects_));
    } else {
        pendingObjects_.clear();
    }
}
