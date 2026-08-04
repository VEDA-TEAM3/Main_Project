#include "ui/panels/DeviceStatusPanel.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

namespace {
constexpr int deviceChannelCount = 4;
constexpr int deviceStatusIconSize = 22;

bool statusesEqual(const DeviceChannelStatus& left, const DeviceChannelStatus& right) {
    return left.channelIndex == right.channelIndex && left.outputs.ledRed == right.outputs.ledRed &&
           left.outputs.ledYellow == right.outputs.ledYellow && left.outputs.ledGreen == right.outputs.ledGreen &&
           left.outputs.beacon == right.outputs.beacon && left.outputs.buzzer == right.outputs.buzzer &&
           left.hasConfirmedState == right.hasConfirmedState && left.sensorHealth == right.sensorHealth &&
           left.feedbackHealth == right.feedbackHealth && left.sensorDetail == right.sensorDetail &&
           left.detail == right.detail && left.confirmedSourceTimestamp == right.confirmedSourceTimestamp;
}

QString channelHealthProperty(const DeviceChannelStatus& status) {
    if (status.feedbackHealth == DeviceFeedbackHealth::Failed) {
        return QStringLiteral("failed");
    }

    if (!status.hasConfirmedState || status.feedbackHealth == DeviceFeedbackHealth::Unknown) {
        return QStringLiteral("unknown");
    }

    return QStringLiteral("confirmed");
}

}  // namespace

/**
 * @brief         4채널 장비 제어/상태 표시 위젯을 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 위젯
 */
DeviceStatusPanel::DeviceStatusPanel(QWidget* parent) : QWidget(parent) {
    channelStatuses_.reserve(deviceChannelCount);

    for (int channelIndex = 0; channelIndex < deviceChannelCount; ++channelIndex) {
        DeviceChannelStatus status;
        status.channelIndex = channelIndex;
        channelStatuses_.append(status);
    }

    setupUi();

    for (int channelIndex = 0; channelIndex < deviceChannelCount; ++channelIndex) {
        updateChannelWidgets(channelIndex);
    }
}

/**
 * @brief        단일 채널 장비 상태를 UI에 반영합니다.
 * @param status  갱신할 채널 상태
 */
void DeviceStatusPanel::setChannelStatus(const DeviceChannelStatus& status) {
    if (status.channelIndex < 0 || status.channelIndex >= channelStatuses_.size()) {
        return;
    }

    if (statusesEqual(channelStatuses_[status.channelIndex], status)) {
        return;
    }

    channelStatuses_[status.channelIndex] = status;
    updateChannelWidgets(status.channelIndex);
}

/**
 * @brief           여러 채널 장비 상태를 한 번에 UI에 반영합니다.
 * @param statuses  갱신할 채널 상태 목록
 */
void DeviceStatusPanel::setChannelStatuses(const QVector<DeviceChannelStatus>& statuses) {
    if (statuses.isEmpty()) {
        return;
    }

    const bool restoreUpdates = updatesEnabled();

    if (restoreUpdates) {
        setUpdatesEnabled(false);
    }

    for (const DeviceChannelStatus& status : statuses) {
        setChannelStatus(status);
    }

    if (restoreUpdates) {
        setUpdatesEnabled(true);
        update();
    }
}

/**
 * @brief   장비 상태 패널의 전체 레이아웃을 구성합니다.
 */
void DeviceStatusPanel::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(0);

    auto* gridLayout = new QGridLayout();
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(8);
    gridLayout->setVerticalSpacing(6);

    for (int channelIndex = 0; channelIndex < deviceChannelCount; ++channelIndex) {
        gridLayout->addWidget(createChannelCard(channelIndex), channelIndex / 2, channelIndex % 2);
    }

    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setRowStretch(0, 1);
    gridLayout->setRowStretch(1, 1);
    rootLayout->addLayout(gridLayout, 1);
}

/**
 * @brief               채널 하나의 LED, 경광등, 부저 상태 카드를 생성합니다.
 * @param channelIndex  생성할 채널 index
 * @return              생성된 채널 카드 frame
 */
QFrame* DeviceStatusPanel::createChannelCard(int channelIndex) {
    ChannelWidgets widgets;
    widgets.card = new QFrame(this);
    widgets.card->setObjectName(QStringLiteral("deviceChannelCard"));

    auto* cardLayout = new QVBoxLayout(widgets.card);
    cardLayout->setContentsMargins(6, 4, 6, 6);
    cardLayout->setSpacing(4);

    widgets.titleLabel =
        new QLabel(QStringLiteral("CH %1").arg(channelIndex + 1, 2, 10, QLatin1Char('0')), widgets.card);
    widgets.titleLabel->setObjectName(QStringLiteral("deviceChannelTitleLabel"));
    cardLayout->addWidget(widgets.titleLabel);

    widgets.ledSafeLabel = createStatusSegment({QStringLiteral("SAFE"), QStringLiteral("safe")});
    widgets.ledWarningLabel = createStatusSegment({QStringLiteral("WARNING"), QStringLiteral("warning")});
    widgets.ledDangerLabel = createStatusSegment({QStringLiteral("DANGER"), QStringLiteral("danger")});
    cardLayout->addWidget(createStatusRow({QStringLiteral(":/icons/led_icon.png"),
                                           QStringLiteral("LED 전광판"),
                                           {widgets.ledSafeLabel, widgets.ledWarningLabel, widgets.ledDangerLabel}}));

    widgets.beaconOffLabel = createStatusSegment({QStringLiteral("OFF"), QStringLiteral("off")});
    widgets.beaconOnLabel = createStatusSegment({QStringLiteral("ON"), QStringLiteral("on")});
    cardLayout->addWidget(createStatusRow({QStringLiteral(":/icons/siren_icon.png"),
                                           QStringLiteral("경광등"),
                                           {widgets.beaconOffLabel, widgets.beaconOnLabel}}));

    widgets.buzzerOffLabel = createStatusSegment({QStringLiteral("OFF"), QStringLiteral("off")});
    widgets.buzzerOnLabel = createStatusSegment({QStringLiteral("ON"), QStringLiteral("on")});
    cardLayout->addWidget(createStatusRow({QStringLiteral(":/icons/buzzer_icon.png"),
                                           QStringLiteral("부저"),
                                           {widgets.buzzerOffLabel, widgets.buzzerOnLabel}}));

    channelWidgets_.append(widgets);

    return widgets.card;
}

/**
 * @brief        아이콘과 상태 선택 segment를 가진 장비 상태 행을 생성합니다.
 * @param spec   아이콘, tooltip, segment를 묶은 행 명세
 * @return       생성된 상태 행 frame
 */
QFrame* DeviceStatusPanel::createStatusRow(const StatusRowSpec& spec) {
    auto* rowFrame = new QFrame(this);
    rowFrame->setObjectName(QStringLiteral("deviceStatusRow"));
    rowFrame->setToolTip(spec.tooltip);

    auto* rowLayout = new QHBoxLayout(rowFrame);
    rowLayout->setContentsMargins(5, 2, 5, 2);
    rowLayout->setSpacing(6);

    auto* iconLabel = new QLabel(rowFrame);
    iconLabel->setObjectName(QStringLiteral("deviceStatusIconLabel"));
    iconLabel->setFixedSize(deviceStatusIconSize, deviceStatusIconSize);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(
        QPixmap(spec.iconPath)
            .scaled(deviceStatusIconSize, deviceStatusIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    rowLayout->addWidget(iconLabel);

    auto* separator = new QFrame(rowFrame);
    separator->setObjectName(QStringLiteral("deviceStatusSeparator"));
    separator->setFixedWidth(1);
    separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    rowLayout->addWidget(separator);

    for (auto* segment : spec.segments) {
        segment->setParent(rowFrame);
        rowLayout->addWidget(segment, 1);
    }

    return rowFrame;
}

/**
 * @brief        상태 segment label을 생성합니다.
 * @param spec   표시 문자열과 QSS 상태 종류를 묶은 segment 명세
 * @return       생성된 segment label
 */
QLabel* DeviceStatusPanel::createStatusSegment(const StatusSegmentSpec& spec) {
    auto* segment = new QLabel(spec.text);
    segment->setObjectName(QStringLiteral("deviceStateSegment"));
    segment->setAlignment(Qt::AlignCenter);
    segment->setMinimumHeight(20);
    segment->setProperty("stateKind", spec.stateKind);
    segment->setProperty("active", false);

    return segment;
}

/**
 * @brief               채널 상태값에 맞춰 각 segment 활성 상태를 갱신합니다.
 * @param channelIndex  갱신할 채널 index
 */
void DeviceStatusPanel::updateChannelWidgets(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= channelStatuses_.size() || channelIndex >= channelWidgets_.size()) {
        return;
    }

    const DeviceChannelStatus& status = channelStatuses_[channelIndex];
    const ChannelWidgets& widgets = channelWidgets_[channelIndex];
    const bool outputsKnown = status.hasConfirmedState;

    setSegmentActive(widgets.ledSafeLabel, outputsKnown && status.outputs.ledGreen);
    setSegmentActive(widgets.ledWarningLabel, outputsKnown && status.outputs.ledYellow);
    setSegmentActive(widgets.ledDangerLabel, outputsKnown && status.outputs.ledRed);
    setSegmentActive(widgets.beaconOffLabel, outputsKnown && !status.outputs.beacon);
    setSegmentActive(widgets.beaconOnLabel, outputsKnown && status.outputs.beacon);
    setSegmentActive(widgets.buzzerOffLabel, outputsKnown && !status.outputs.buzzer);
    setSegmentActive(widgets.buzzerOnLabel, outputsKnown && status.outputs.buzzer);

    const QString healthProperty = channelHealthProperty(status);
    if (widgets.card->property("healthState").toString() != healthProperty) {
        widgets.card->setProperty("healthState", healthProperty);
        widgets.card->style()->unpolish(widgets.card);
        widgets.card->style()->polish(widgets.card);
        widgets.card->update();
    }

    if (status.feedbackHealth == DeviceFeedbackHealth::Failed) {
        widgets.card->setToolTip(QStringLiteral("마지막 확정 상태 표시 중\n상태 확인 실패: %1").arg(status.detail));
    } else if (status.feedbackHealth == DeviceFeedbackHealth::Confirmed) {
        widgets.card->setToolTip(QStringLiteral("장비 출력 피드백 확인됨"));
    } else {
        widgets.card->setToolTip(QStringLiteral("장비 상태 미수신"));
    }
}

/**
 * @brief         상태 segment의 현재 활성 여부를 QSS 속성으로 반영합니다.
 * @param label   갱신할 segment label
 * @param active  현재 상태이면 true
 */
void DeviceStatusPanel::setSegmentActive(QLabel* label, bool active) {
    if (!label) {
        return;
    }

    if (label->property("active").toBool() == active) {
        return;
    }

    label->setProperty("active", active);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
