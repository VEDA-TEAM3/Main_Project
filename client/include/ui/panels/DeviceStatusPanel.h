#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include "model/DeviceStatus.h"

class QQuickWidget;

class DeviceStatusPanel final : public QWidget {
    Q_OBJECT

public:
    explicit DeviceStatusPanel(QWidget* parent = nullptr);

    void setChannelCount(int channelCount);
    void setAreaIndex(int areaIndex);
    void setChannelStatus(const DeviceChannelStatus& status);
    void setChannelStatuses(const QVector<DeviceChannelStatus>& statuses);

private:
    void setupUi();
    bool storeChannelStatus(const DeviceChannelStatus& status);
    void refreshChannels();

    QQuickWidget* view_ = nullptr;
    QVector<DeviceChannelStatus> channelStatuses_;
    int areaIndex_ = 0;
};
