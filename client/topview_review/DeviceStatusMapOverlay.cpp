#include "overlays/DeviceStatusMapOverlay.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QPointF>
#include <QString>
#include <QtGlobal>

namespace {
constexpr int channelCount = 8;
constexpr int channelsPerZone = 4;
constexpr int iconSize = 34;
constexpr int centralCctvIconSize = 66;
constexpr double iconGap = 5.0;
constexpr double centralCctvZValue = 3.0;
constexpr double overlayZValue = 40.0;
}  // namespace

/**
 * @brief           두 지도에 CCTV와 8채널 장치 상태 아이콘을 배치합니다.
 * @param scene     장치 아이콘을 표시할 scene
 * @param zoneRects 물리 CCTV별 지도 영역
 */
void DeviceStatusMapOverlay::initialize(QGraphicsScene* scene, const std::array<QRectF, 2>& zoneRects) {
    if (!scene) {
        return;
    }

    loadPixmaps();
    for (int zoneIndex = 0; zoneIndex < static_cast<int>(zoneRects.size()); ++zoneIndex) {
        cctvItems_[zoneIndex] = scene->addPixmap(cctvPixmap_);
        cctvItems_[zoneIndex]->setPos(zoneRects[zoneIndex].center().x() - cctvPixmap_.width() / 2.0,
                                      zoneRects[zoneIndex].center().y() - cctvPixmap_.height() / 2.0);
        cctvItems_[zoneIndex]->setZValue(centralCctvZValue);
        cctvItems_[zoneIndex]->setTransformationMode(Qt::SmoothTransformation);
    }

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        const int zoneIndex = channelIndex / channelsPerZone;
        const int localChannelIndex = channelIndex % channelsPerZone;
        const QRectF& zoneRect = zoneRects[zoneIndex];
        const std::array<QPointF, channelsPerZone> anchors = {
            QPointF(zoneRect.center().x(), zoneRect.top() + 30.0),
            QPointF(zoneRect.right() - 52.0, zoneRect.center().y()),
            QPointF(zoneRect.center().x(), zoneRect.bottom() - 30.0),
            QPointF(zoneRect.left() + 52.0, zoneRect.center().y()),
        };
        const QPointF anchor = anchors[localChannelIndex];
        const double pairWidth = iconSize * 2.0 + iconGap;
        const double left = anchor.x() - pairWidth / 2.0;
        const double top = anchor.y() - iconSize / 2.0;

        ChannelVisualItems& items = channels_[channelIndex];
        items.led = scene->addPixmap(ledOffPixmap_);
        items.led->setPos(left, top);
        items.led->setZValue(overlayZValue);
        items.led->setTransformationMode(Qt::SmoothTransformation);

        items.sensor = scene->addPixmap(sensorOffPixmap_);
        items.sensor->setPos(left + iconSize + iconGap, top);
        items.sensor->setZValue(overlayZValue);
        items.sensor->setTransformationMode(Qt::SmoothTransformation);
    }

    updateAllChannels();
}

/** @brief MQTT 연결 상태를 장치 아이콘 유효성에 반영합니다. */
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

/** @brief 수신된 0 기반 채널 상태를 해당 지도 아이콘에 반영합니다. */
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

/** @brief 지도 장치 아이콘 표시 설정을 적용합니다. */
void DeviceStatusMapOverlay::setDisplaySettings(const DigitalTwinMapDisplaySettings& settings) {
    displaySettings_ = settings;
    updateAllChannels();
}

/** @brief 장치 상태 아이콘 리소스를 한 번만 준비합니다. */
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

/** @brief 모든 장치 아이콘을 현재 상태로 다시 표시합니다. */
void DeviceStatusMapOverlay::updateAllChannels() {
    for (QGraphicsPixmapItem* cctvItem : cctvItems_) {
        if (cctvItem) {
            cctvItem->setVisible(displaySettings_.showCctv);
        }
    }

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        updateChannel(channelIndex);
    }
}

/** @brief 한 채널의 LED와 통합 알림 장치 아이콘을 갱신합니다. */
void DeviceStatusMapOverlay::updateChannel(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= channelCount) {
        return;
    }

    ChannelVisualItems& items = channels_[channelIndex];
    if (!items.led || !items.sensor) {
        return;
    }

    items.led->setVisible(displaySettings_.showLed);
    items.sensor->setVisible(displaySettings_.showAlertDevice);
    if (!hasValidSignal(items)) {
        items.led->setPixmap(ledOffPixmap_);
        items.sensor->setPixmap(sensorOffPixmap_);
        return;
    }

    items.led->setPixmap(ledPixmap(items.status.outputs));
    const bool alarmActive = items.status.outputs.beacon || items.status.outputs.buzzer;
    items.sensor->setPixmap(alarmActive ? sensorActivePixmap_ : sensorSafePixmap_);
}

/** @brief 현재 세션에서 확인된 유효한 하드웨어 피드백인지 검사합니다. */
bool DeviceStatusMapOverlay::hasValidSignal(const ChannelVisualItems& items) const {
    return signalAvailable_ && items.hasStatus && items.receivedInCurrentSession && items.status.hasConfirmedState &&
           items.status.feedbackHealth == DeviceFeedbackHealth::Confirmed;
}

/** @brief LED 출력 비트에서 가장 높은 우선순위의 아이콘을 반환합니다. */
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

/** @brief Qt 리소스 이미지를 지도용 크기로 변환합니다. */
QPixmap DeviceStatusMapOverlay::loadScaledPixmap(const QString& resourcePath, int size) const {
    return QPixmap(resourcePath).scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
