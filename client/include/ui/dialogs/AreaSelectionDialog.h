#pragma once

#include <QStringList>
#include <QWidget>

class QListWidget;
class QShowEvent;

class AreaSelectionDialog final : public QWidget {
    Q_OBJECT

public:
    explicit AreaSelectionDialog(QWidget* parent = nullptr);

    void setAreas(const QStringList& areaNames, int currentAreaIndex);
    void setCurrentAreaIndex(int currentAreaIndex);

signals:
    void areaSelected(int areaIndex);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void confirmSelection();

    QListWidget* areaList_ = nullptr;
};
