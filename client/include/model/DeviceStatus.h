#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

enum class DeviceFeedbackHealth {
    Unknown,
    Confirmed,
    Failed,
};

enum class SensorHealth {
    Unknown,
    Online,
    Offline,
};

struct DeviceOutputState {
    bool ledRed = false;
    bool ledYellow = false;
    bool ledGreen = false;
    bool beacon = false;
    bool buzzer = false;

    bool operator==(const DeviceOutputState& other) const = default;
};

struct DeviceChannelStatus {
    int channelIndex = 0;
    DeviceOutputState outputs;
    bool hasConfirmedState = false;
    SensorHealth sensorHealth = SensorHealth::Unknown;
    DeviceFeedbackHealth feedbackHealth = DeviceFeedbackHealth::Unknown;
    QString sensorDetail;
    QString detail;
    qint64 confirmedSourceTimestamp = 0;
};

/**
 * @brief       화면에 표시되는 장비 상태가 같은지 확인합니다.
 * @param left  비교할 채널 상태
 * @param right 비교할 채널 상태
 * @return      표시값이 모두 같으면 true
 *
 * @details confirmedSourceTimestamp는 상태 최신성 판단용이며 화면에 나오지 않으므로 빼고 봅니다.
 *          그래서 operator== 기본 구현을 쓸 수 없습니다. 패널과 service가 각자 같은 함수를
 *          들고 있었는데, 한쪽만 필드를 추가하면 그 필드의 변화가 화면에 반영되지 않습니다.
 */
inline bool hasSameDisplayedState(const DeviceChannelStatus& left, const DeviceChannelStatus& right) {
    return left.channelIndex == right.channelIndex && left.outputs == right.outputs &&
           left.hasConfirmedState == right.hasConfirmedState && left.sensorHealth == right.sensorHealth &&
           left.feedbackHealth == right.feedbackHealth && left.sensorDetail == right.sensorDetail &&
           left.detail == right.detail;
}

Q_DECLARE_METATYPE(DeviceOutputState)
Q_DECLARE_METATYPE(DeviceChannelStatus)
Q_DECLARE_METATYPE(QVector<DeviceChannelStatus>)
