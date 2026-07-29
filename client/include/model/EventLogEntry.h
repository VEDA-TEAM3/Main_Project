#pragma once

#include <QMetaType>
#include <QString>
#include <QTime>

enum class EventLogRiskLevel {
    Normal,
    Warning,
    Danger,
};

enum class EventLogAction {
    None,
    WarningAlertActivated,
    DangerAlertActivated,
};

enum class EventLogSource {
    ObjectDetected,
    ObjectProximity,
};

struct EventLogEntry {
    QTime time;
    QString area;
    QString objectText;
    EventLogRiskLevel riskLevel = EventLogRiskLevel::Normal;
    EventLogAction action = EventLogAction::None;
    EventLogSource source = EventLogSource::ObjectDetected;
};

Q_DECLARE_METATYPE(EventLogEntry)
