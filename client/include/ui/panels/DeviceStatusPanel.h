#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include "model/DeviceStatus.h"

class QFrame;
class QLabel;

class DeviceStatusPanel final : public QWidget {
    Q_OBJECT

public:
    explicit DeviceStatusPanel(QWidget* parent = nullptr);

    void setChannelStatus(const DeviceChannelStatus& status);
    void setChannelStatuses(const QVector<DeviceChannelStatus>& statuses);

private:
    struct StatusSegmentSpec {
        QString text;
        QString stateKind;
    };

    struct StatusRowSpec {
        QString iconPath;
        QString tooltip;
        QVector<QLabel*> segments;
    };

    struct ChannelWidgets {
        QFrame* card = nullptr;
        QLabel* titleLabel = nullptr;
        QLabel* ledSafeLabel = nullptr;
        QLabel* ledWarningLabel = nullptr;
        QLabel* ledDangerLabel = nullptr;
        QLabel* beaconOffLabel = nullptr;
        QLabel* beaconOnLabel = nullptr;
        QLabel* buzzerOffLabel = nullptr;
        QLabel* buzzerOnLabel = nullptr;
    };

    void setupUi();
    QFrame* createChannelCard(int channelIndex);
    QFrame* createStatusRow(const StatusRowSpec& spec);
    QLabel* createStatusSegment(const StatusSegmentSpec& spec);
    void updateChannelWidgets(int channelIndex);
    void setSegmentActive(QLabel* label, bool active);

    QVector<DeviceChannelStatus> channelStatuses_;
    QVector<ChannelWidgets> channelWidgets_;
};
