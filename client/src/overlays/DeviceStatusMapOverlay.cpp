#include "overlays/DeviceStatusMapOverlay.h"

#include <QColor>
#include <QFont>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QPointF>
#include <QString>
#include <QtGlobal>

namespace {
constexpr int channelCount = 4;
constexpr int iconSize = 38;
constexpr int centralCctvIconSize = 74;
constexpr double iconGap = 5.0;
constexpr double centralCctvZValue = 3.0;
constexpr double overlayZValue = 40.0;
const QPointF centralCctvPosition(500.0, 260.0);

const std::array<QPointF, channelCount> channelAnchors = {
    QPointF(106.0, 100.0),
    QPointF(544.0, 100.0),
    QPointF(454.0, 420.0),
    QPointF(910.0, 420.0),
};
}  // namespace

/**
 * @brief        맵 장면에 채널별 LED와 통합 알림 장치 아이콘을 배치합니다.
 * @param scene  장치 아이콘을 표시할 디지털 트윈 장면
 */
void DeviceStatusMapOverlay::initialize(QGraphicsScene* scene) {
    if (!scene) {
        return;
    }

    loadPixmaps();

    cctvItem_ = scene->addPixmap(cctvPixmap_);
    cctvItem_->setPos(centralCctvPosition.x() - cctvPixmap_.width() / 2.0,
                      centralCctvPosition.y() - cctvPixmap_.height() / 2.0);
    cctvItem_->setZValue(centralCctvZValue);
    cctvItem_->setTransformationMode(Qt::SmoothTransformation);

    QFont channelLabelFont(QStringLiteral("Segoe UI"));
    channelLabelFont.setPointSizeF(9.0);
    channelLabelFont.setBold(true);

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        ChannelVisualItems& items = channels_[channelIndex];
        const QPointF anchor = channelAnchors[channelIndex];
        const double pairWidth = iconSize * 2.0 + iconGap;
        const double left = anchor.x() - pairWidth / 2.0;
        const double top = anchor.y() - iconSize / 2.0;

        items.led = scene->addPixmap(ledOffPixmap_);
        items.led->setPos(left, top);
        items.led->setZValue(overlayZValue);
        items.led->setTransformationMode(Qt::SmoothTransformation);

        items.sensor = scene->addPixmap(sensorOffPixmap_);
        items.sensor->setPos(left + iconSize + iconGap, top);
        items.sensor->setZValue(overlayZValue);
        items.sensor->setTransformationMode(Qt::SmoothTransformation);

        items.channelLabel =
            scene->addSimpleText(QStringLiteral("CH %1").arg(channelIndex + 1, 2, 10, QChar('0')), channelLabelFont);
        items.channelLabel->setBrush(QColor(QStringLiteral("#65baff")));
        items.channelLabel->setZValue(overlayZValue + 1.0);
        const QRectF labelBounds = items.channelLabel->boundingRect();
        items.channelLabel->setPos(anchor.x() - labelBounds.width() / 2.0, top - labelBounds.height() - 4.0);
    }

    updateAllChannels();
}

/**
 * @brief            MQTT 연결 여부를 반영하고 연결이 끊기면 모든 장치를 무신호
 * 상태로 표시합니다.
 * @param available  장치 상태 신호를 수신할 수 있으면 true
 */
void DeviceStatusMapOverlay::setSignalAvailable(bool available) {
    if (signalAvailable_ == available) {
        return;
    }

    signalAvailable_ = available;

    if (!available) {
        for (ChannelVisualItems& items : channels_) {
            items.receivedInCurrentSession = false;
        }
    }

    updateAllChannels();
}

/**
 * @brief           수신된 채널 상태를 저장하고 해당 장치 아이콘만 갱신합니다.
 * @param statuses  이번 UI 주기에 변경된 채널 상태 목록
 */
void DeviceStatusMapOverlay::setChannelStatuses(const QVector<DeviceChannelStatus>& statuses) {
    for (const DeviceChannelStatus& status : statuses) {
        if (status.channelIndex < 0 || status.channelIndex >= channelCount) {
            continue;
        }

        ChannelVisualItems& items = channels_[status.channelIndex];
        items.status = status;
        items.hasStatus = true;

        if (signalAvailable_ && status.hasConfirmedState && status.feedbackHealth == DeviceFeedbackHealth::Confirmed) {
            items.receivedInCurrentSession = true;
        }

        updateChannel(status.channelIndex);
    }
}

/**
 * @brief          LED, 알림 장치와 중앙 CCTV 아이콘의 표시 옵션을 적용합니다.
 * @param settings 설정 팝업의 맵 표시 설정
 */
void DeviceStatusMapOverlay::setDisplaySettings(const DigitalTwinMapDisplaySettings& settings) {
    displaySettings_ = settings;
    updateAllChannels();
}

/**
 * @brief 장치 상태 아이콘 리소스를 맵 표시 크기로 한 번만 준비합니다.
 */
void DeviceStatusMapOverlay::loadPixmaps() {
    if (!ledOffPixmap_.isNull()) {
        return;
    }

    ledOffPixmap_ = loadScaledPixmap(QStringLiteral(":/icons/led_off.png"), iconSize);
    ledSafePixmap_ = loadScaledPixmap(QStringLiteral(":/icons/led_safe.png"), iconSize);
    ledWarningPixmap_ = loadScaledPixmap(QStringLiteral(":/icons/led_waring.png"), iconSize);
    ledDangerPixmap_ = loadScaledPixmap(QStringLiteral(":/icons/led_danger.png"), iconSize);
    sensorOffPixmap_ = loadScaledPixmap(QStringLiteral(":/icons/sensor_off.png"), iconSize);
    sensorSafePixmap_ = loadScaledPixmap(QStringLiteral(":/icons/sensor_safe.png"), iconSize);
    sensorActivePixmap_ = loadScaledPixmap(QStringLiteral(":/icons/sensor_danger.png"), iconSize);
    cctvPixmap_ = loadScaledPixmap(QStringLiteral(":/icons/cctv_icon.png"), centralCctvIconSize);
}

/**
 * @brief 모든 채널의 장치 아이콘을 현재 상태로 다시 그립니다.
 */
void DeviceStatusMapOverlay::updateAllChannels() {
    if (cctvItem_) {
        cctvItem_->setVisible(displaySettings_.showCctv);
    }

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        updateChannel(channelIndex);
    }
}

/**
 * @brief               한 채널의 LED와 통합 알림 장치 아이콘을 갱신합니다.
 * @param channelIndex  갱신할 0 기준 채널 번호
 */
void DeviceStatusMapOverlay::updateChannel(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= channelCount) {
        return;
    }

    ChannelVisualItems& items = channels_[channelIndex];
    if (!items.led || !items.sensor || !items.channelLabel) {
        return;
    }

    items.led->setVisible(displaySettings_.showLed);
    items.sensor->setVisible(displaySettings_.showAlertDevice);
    items.channelLabel->setVisible(displaySettings_.showLed || displaySettings_.showAlertDevice);

    if (!hasValidSignal(items)) {
        items.led->setPixmap(ledOffPixmap_);
        items.sensor->setPixmap(sensorOffPixmap_);
        return;
    }

    items.led->setPixmap(ledPixmap(items.status.outputs));
    const bool alarmActive = items.status.outputs.beacon || items.status.outputs.buzzer;
    items.sensor->setPixmap(alarmActive ? sensorActivePixmap_ : sensorSafePixmap_);
}

/**
 * @brief        현재 MQTT 연결에서 확인된 유효 장치 상태인지 검사합니다.
 * @param items  검사할 채널 장치 표시 정보
 * @return       현재 세션의 확정 상태이면 true
 */
bool DeviceStatusMapOverlay::hasValidSignal(const ChannelVisualItems& items) const {
    return signalAvailable_ && items.hasStatus && items.receivedInCurrentSession && items.status.hasConfirmedState &&
           items.status.feedbackHealth == DeviceFeedbackHealth::Confirmed;
}

/**
 * @brief         LED 출력 비트의 우선순위에 맞는 상태 아이콘을 반환합니다.
 * @param outputs  확인된 실제 장치 출력 상태
 * @return         위험, 주의, 정상 또는 꺼짐 LED 아이콘
 */
const QPixmap& DeviceStatusMapOverlay::ledPixmap(const DeviceOutputState& outputs) const {
    if (outputs.ledRed) {
        return ledDangerPixmap_;
    }
    if (outputs.ledYellow) {
        return ledWarningPixmap_;
    }
    if (outputs.ledGreen) {
        return ledSafePixmap_;
    }
    return ledOffPixmap_;
}

/**
 * @brief               아이콘 리소스를 맵 오버레이 크기로 변환합니다.
 * @param resourcePath  Qt 리소스 경로
 * @return              부드럽게 축소된 아이콘 이미지
 */
QPixmap DeviceStatusMapOverlay::loadScaledPixmap(const QString& resourcePath, int size) const {
    return QPixmap(resourcePath).scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
