#pragma once

#include <QHash>
#include <QSet>
#include <QVector>

#include "model/DigitalTwinTypes.h"
#include "model/EventLogEntry.h"

class EventLogGenerator final {
public:
    QVector<EventLogEntry> createEntriesFromSnapshot(const DigitalTwinSnapshot& snapshot);
    void reset();

private:
    QSet<QString> knownObjectIds_;
    QHash<QString, EventLogRiskLevel> activePairRiskLevels_;
};
