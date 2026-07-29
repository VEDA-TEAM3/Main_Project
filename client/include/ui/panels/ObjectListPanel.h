#pragma once

#include <QVector>
#include <QWidget>

#include "model/DigitalTwinTypes.h"

class DigitalTwinObjectTableModel;
class QResizeEvent;
class QStyledItemDelegate;
class QTableView;

class ObjectListPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectListPanel(QWidget* parent = nullptr);

    void setObjects(QVector<DigitalTwinObject> objects);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUi();
    void resizeColumns();

    DigitalTwinObjectTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStyledItemDelegate* objectTypeDelegate_ = nullptr;
};
