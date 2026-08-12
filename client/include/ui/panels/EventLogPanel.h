#pragma once

#include <QVector>
#include <QWidget>

#include "model/EventLogEntry.h"

class EventLogTableModel;

class EventLogPanel final : public QWidget {
    Q_OBJECT

public:
    explicit EventLogPanel(QWidget* parent = nullptr);

    void prependEntry(const EventLogEntry& entry);
    void prependEntries(QVector<EventLogEntry> entries);
    void clear();

private:
    void setupUi();

    EventLogTableModel* model_ = nullptr;
};
