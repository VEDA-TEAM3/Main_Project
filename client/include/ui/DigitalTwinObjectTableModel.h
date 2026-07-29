#pragma once

#include <QAbstractTableModel>
#include <QVector>

#include "model/DigitalTwinTypes.h"

class DigitalTwinObjectTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit DigitalTwinObjectTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void updateObjects(QVector<DigitalTwinObject> objects);

private:
    enum ObjectListColumn {
        ObjectIdColumn = 0,
        ObjectTypeColumn,
        ObjectPositionColumn,
        ObjectAreaColumn,
        ObjectListColumnCount,
    };

    bool hasSameIdentityOrder(const QVector<DigitalTwinObject>& objects) const;

    QVector<DigitalTwinObject> objects_;
};
