#pragma once

#include <QAbstractTableModel>
#include <QVector>

#include "model/EventLogEntry.h"

class EventLogTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit EventLogTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void prependEntry(const EventLogEntry& entry);
    void prependEntries(QVector<EventLogEntry> entries);
    void clear();

private:
    enum EventLogColumn {
        EventTimeColumn = 0,
        EventAreaColumn,
        EventObjectColumn,
        EventRiskColumn,
        EventActionColumn,
        EventLogColumnCount,
    };

    QVector<EventLogEntry> entries_;
};
