#include "ui/panels/DeviceStatusPanel.h"

#include <QQuickItem>
#include <QQuickWidget>
#include <QVBoxLayout>
#include <QVariantList>

#include "ui/SharedQmlEngine.h"

namespace {
constexpr int visibleChannelCount = 4;

/**
 * @brief         카드 테두리 색을 고르는 데 쓰는 상태 이름을 반환합니다.
 * @param status  대상 채널 상태
 * @return        confirmed, failed 또는 unknown
 */
QString channelHealthState(const DeviceChannelStatus& status) {
    if (status.feedbackHealth == DeviceFeedbackHealth::Failed) {
        return QStringLiteral("failed");
    }

    if (!status.hasConfirmedState || status.feedbackHealth == DeviceFeedbackHealth::Unknown) {
        return QStringLiteral("unknown");
    }

    return QStringLiteral("confirmed");
}

/**
 * @brief         카드에 표시할 안내 문구를 반환합니다.
 * @param status  대상 채널 상태
 * @return        피드백 상태 설명
 */
QString channelTooltip(const DeviceChannelStatus& status) {
    if (status.feedbackHealth == DeviceFeedbackHealth::Failed) {
        return QStringLiteral("마지막 확정 상태 표시 중\n상태 확인 실패: %1").arg(status.detail);
    }

    if (status.feedbackHealth == DeviceFeedbackHealth::Confirmed) {
        return QStringLiteral("장비 출력 피드백 확인됨");
    }

    return QStringLiteral("장비 상태 미수신");
}
}  // namespace

/**
 * @brief         4채널 장비 제어/상태 표시 위젯을 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 위젯
 */
DeviceStatusPanel::DeviceStatusPanel(QWidget* parent) : QWidget(parent) {
    setChannelCount(visibleChannelCount);
    setupUi();
    refreshChannels();
}

/**
 * @brief              패널이 보관할 전체 장비 채널 수를 설정합니다.
 * @param channelCount 설정된 전체 채널 수
 */
void DeviceStatusPanel::setChannelCount(int channelCount) {
    const int normalizedChannelCount = qMax(visibleChannelCount, channelCount);
    if (channelStatuses_.size() == normalizedChannelCount) {
        return;
    }

    channelStatuses_.resize(normalizedChannelCount);
    for (int channelIndex = 0; channelIndex < normalizedChannelCount; ++channelIndex) {
        channelStatuses_[channelIndex].channelIndex = channelIndex;
    }

    areaIndex_ = qMin(areaIndex_, normalizedChannelCount / visibleChannelCount - 1);
    refreshChannels();
}

/**
 * @brief            패널에 표시할 CCTV 구역을 변경합니다.
 * @param areaIndex  0부터 시작하는 구역 인덱스
 */
void DeviceStatusPanel::setAreaIndex(int areaIndex) {
    if (areaIndex < 0 || areaIndex >= channelStatuses_.size() / visibleChannelCount || areaIndex_ == areaIndex) {
        return;
    }

    areaIndex_ = areaIndex;
    refreshChannels();
}

/**
 * @brief         단일 채널 장비 상태를 보관합니다.
 * @param status  갱신할 채널 상태
 * @return        현재 보이는 구역의 값이 실제로 바뀌었으면 true
 */
bool DeviceStatusPanel::storeChannelStatus(const DeviceChannelStatus& status) {
    if (status.channelIndex < 0 || status.channelIndex >= channelStatuses_.size()) {
        return false;
    }

    if (hasSameDisplayedState(channelStatuses_[status.channelIndex], status)) {
        return false;
    }

    channelStatuses_[status.channelIndex] = status;
    return status.channelIndex / visibleChannelCount == areaIndex_;
}

/**
 * @brief           여러 채널 장비 상태를 한 번에 UI에 반영합니다.
 * @param statuses  갱신할 채널 상태 목록
 */
void DeviceStatusPanel::setChannelStatuses(const QVector<DeviceChannelStatus>& statuses) {
    bool visibleChanged = false;
    for (const DeviceChannelStatus& status : statuses) {
        visibleChanged = storeChannelStatus(status) || visibleChanged;
    }

    if (visibleChanged) {
        refreshChannels();
    }
}

/**
 * @brief   장비 상태 QML 뷰를 패널에 배치합니다.
 */
void DeviceStatusPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    view_ = createQmlPanelView(QStringLiteral("DeviceStatusView.qml"), this);
    if (view_) {
        layout->addWidget(view_);
    }
}

/**
 * @brief   현재 구역의 네 채널 상태를 QML이 읽는 배열로 만들어 넘깁니다.
 */
void DeviceStatusPanel::refreshChannels() {
    QVariantList channels;
    for (int localChannelIndex = 0; localChannelIndex < visibleChannelCount; ++localChannelIndex) {
        const int globalChannelIndex = areaIndex_ * visibleChannelCount + localChannelIndex;
        const DeviceChannelStatus status = channelStatuses_.value(globalChannelIndex);
        const bool outputsKnown = status.hasConfirmedState;

        // ponytail: QVariantMap을 담으면 heap이 깨져서(자세한 내용은 CLAUDE.md) 평평한 배열로 넘깁니다.
        // 자리 순서는 DeviceStatusView.qml의 readonly 속성과 짝을 맞춰야 합니다.
        // QVariant로 감싸지 않으면 QList::append(const QList&) 오버로드가 골라져 통째로 펼쳐집니다.
        channels.append(QVariant(
            QVariantList{QStringLiteral("CH %1").arg(localChannelIndex + 1, 2, 10, QLatin1Char('0')),
                         channelHealthState(status), channelTooltip(status), outputsKnown && status.outputs.ledGreen,
                         outputsKnown && status.outputs.ledYellow, outputsKnown && status.outputs.ledRed,
                         outputsKnown && !status.outputs.beacon, outputsKnown && status.outputs.beacon,
                         outputsKnown && !status.outputs.buzzer, outputsKnown && status.outputs.buzzer}));
    }

    if (view_ && view_->rootObject()) {
        view_->rootObject()->setProperty("channels", channels);
    }
}
