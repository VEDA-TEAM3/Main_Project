#pragma once

#include <QVector>
#include <QWidget>

#include "model/EventLogEntry.h"

class EventLogTableModel;
class QResizeEvent;
class QTableView;

class EventLogPanel final : public QWidget {
    Q_OBJECT

public:
    explicit EventLogPanel(QWidget* parent = nullptr);

    void prependEntry(const EventLogEntry& entry);
    void prependEntries(QVector<EventLogEntry> entries);
    void clear();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUi();
    void resizeColumns();
    void updateScrollHeaderCover();

    EventLogTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QWidget* scrollHeaderCover_ = nullptr;
};
