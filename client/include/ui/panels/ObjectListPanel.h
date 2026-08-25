#pragma once

#include <QVector>
#include <QWidget>

#include "model/DigitalTwinTypes.h"

class DigitalTwinObjectTableModel;

class ObjectListPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectListPanel(QWidget* parent = nullptr);

    void setObjects(QVector<DigitalTwinObject> objects);

private:
    void setupUi();

    DigitalTwinObjectTableModel* model_ = nullptr;
};
