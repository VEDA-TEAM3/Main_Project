#include "overlays/DeviceStatusMapOverlay.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QPointF>
#include <QString>
#include <QtGlobal>

namespace {
constexpr int channelsPerZone = 4;
constexpr int iconSize = 24;
constexpr int centralCctvIconSize = 40;
constexpr double iconGap = 4.0;
/// 상태 칩 안쪽 여백. DigitalTwinMapSceneBuilder가 그리는 칩과 맞춰야 한다
constexpr double chipPadding = 8.0;
constexpr double centralCctvZValue = 3.0;
constexpr double overlayZValue = 40.0;
}  // namespace

/**
 * @brief                  지도에 CCTV 위치와 구역별 장치 상태 아이콘을 배치합니다.
 * @param scene            장치 아이콘을 표시할 scene
 * @param zoneRects        물리 CCTV별 지도 영역
 * @param zoneStatusSlots  구역별 장치 상태 칩 자리 (도면 위·아래 여백)
 *
 * @details 채널마다 아이콘을 뿌리면 도면 위가 아이콘으로 뒤덮여 객체와 구획선이 묻힌다.
 *          그래서 아이콘은 구역당 한 쌍만 두고 그 구역 채널들의 상태를 집약해서 보여 준다.
 */
void DeviceStatusMapOverlay::initialize(QGraphicsScene* scene, const QVector<QRectF>& zoneRects,
                                        const QVector<QRectF>& zoneStatusSlots) {
    if (!scene || zoneRects.size() != zoneStatusSlots.size()) {
        return;
    }

    // scene->clear()가 이전 아이템을 이미 지웠으므로 포인터만 새로 잡는다
    const int zoneCount = static_cast<int>(zoneRects.size());
    zones_.fill(ZoneVisualItems{}, zoneCount);
    cctvItems_.fill(nullptr, zoneCount);
    channels_.resize(zoneCount * channelsPerZone);

    loadPixmaps();
    for (int zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex) {
        cctvItems_[zoneIndex] = scene->addPixmap(cctvPixmap_);
        cctvItems_[zoneIndex]->setPos(zoneRects[zoneIndex].center().x() - cctvPixmap_.width() / 2.0,
                                      zoneRects[zoneIndex].center().y() - cctvPixmap_.height() / 2.0);
        cctvItems_[zoneIndex]->setZValue(centralCctvZValue);
        cctvItems_[zoneIndex]->setTransformationMode(Qt::SmoothTransformation);

        // 칩 오른쪽에 LED, 통합 알림 순으로 붙인다. 왼쪽 여백은 도면이 그린 구역 이름표 자리다
        const QRectF& slot = zoneStatusSlots[zoneIndex];
        const double top = slot.center().y() - iconSize / 2.0;
        const double sensorLeft = slot.right() - chipPadding - iconSize;
        const double ledLeft = sensorLeft - iconGap - iconSize;

        ZoneVisualItems& items = zones_[zoneIndex];
        items.led = scene->addPixmap(ledOffPixmap_);
        items.led->setPos(ledLeft, top);
        items.led->setZValue(overlayZValue);
        items.led->setTransformationMode(Qt::SmoothTransformation);

        items.sensor = scene->addPixmap(sensorOffPixmap_);
        items.sensor->setPos(sensorLeft, top);
        items.sensor->setZValue(overlayZValue);
        items.sensor->setTransformationMode(Qt::SmoothTransformation);
    }

    updateAllZones();
}

/** @brief MQTT 연결 상태를 장치 아이콘 유효성에 반영합니다. */
void DeviceStatusMapOverlay::setSignalAvailable(bool available) {
    if (signalAvailable_ == available) {
        return;
    }

    signalAvailable_ = available;
    if (!available) {
        for (ChannelStatusRecord& record : channels_) {
            record.receivedInCurrentSession = false;
        }
    }
    updateAllZones();
}

/** @brief 수신된 0 기반 채널 상태를 해당 지도 아이콘에 반영합니다. */
void DeviceStatusMapOverlay::setChannelStatuses(const QVector<DeviceChannelStatus>& statuses) {
    for (const DeviceChannelStatus& status : statuses) {
        if (status.channelIndex < 0 || status.channelIndex >= channels_.size()) {
            continue;
        }

        ChannelStatusRecord& record = channels_[status.channelIndex];
        record.status = status;
        record.hasStatus = true;
        if (signalAvailable_ && status.hasConfirmedState && status.feedbackHealth == DeviceFeedbackHealth::Confirmed) {
            record.receivedInCurrentSession = true;
        }
        updateZone(status.channelIndex / channelsPerZone);
    }
}

/** @brief 지도 장치 아이콘 표시 설정을 적용합니다. */
void DeviceStatusMapOverlay::setDisplaySettings(const DigitalTwinMapDisplaySettings& settings) {
    displaySettings_ = settings;
    updateAllZones();
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

/** @brief 모든 구역 아이콘을 현재 상태로 다시 표시합니다. */
void DeviceStatusMapOverlay::updateAllZones() {
    for (QGraphicsPixmapItem* cctvItem : cctvItems_) {
        if (cctvItem) {
            cctvItem->setVisible(displaySettings_.showCctv);
        }
    }

    for (int zoneIndex = 0; zoneIndex < static_cast<int>(zones_.size()); ++zoneIndex) {
        updateZone(zoneIndex);
    }
}

/**
 * @brief            한 구역의 LED와 통합 알림 아이콘을 갱신합니다.
 * @param zoneIndex  물리 CCTV 구역 인덱스
 *
 * @details 구역 안 채널 중 하나라도 켜져 있으면 켜진 것으로 본다. LED는 가장 높은 위험 단계를
 *          따르고(빨강 > 노랑 > 초록), 통합 알림은 경광등이나 부저가 하나라도 울리면 활성이다.
 *          유효한 피드백이 하나도 없는 구역만 꺼짐으로 표시한다.
 */
void DeviceStatusMapOverlay::updateZone(int zoneIndex) {
    if (zoneIndex < 0 || zoneIndex >= static_cast<int>(zones_.size())) {
        return;
    }

    ZoneVisualItems& items = zones_[zoneIndex];
    if (!items.led || !items.sensor) {
        return;
    }

    items.led->setVisible(displaySettings_.showLed);
    items.sensor->setVisible(displaySettings_.showAlertDevice);

    DeviceOutputState aggregated;
    bool anyValid = false;
    bool alarmActive = false;
    for (int localChannel = 0; localChannel < channelsPerZone; ++localChannel) {
        const int channelIndex = zoneIndex * channelsPerZone + localChannel;
        if (channelIndex >= channels_.size()) {
            break;
        }

        const ChannelStatusRecord& record = channels_[channelIndex];
        if (!hasValidSignal(record)) {
            continue;
        }

        anyValid = true;
        aggregated.ledRed = aggregated.ledRed || record.status.outputs.ledRed;
        aggregated.ledYellow = aggregated.ledYellow || record.status.outputs.ledYellow;
        aggregated.ledGreen = aggregated.ledGreen || record.status.outputs.ledGreen;
        alarmActive = alarmActive || record.status.outputs.beacon || record.status.outputs.buzzer;
    }

    if (!anyValid) {
        items.led->setPixmap(ledOffPixmap_);
        items.sensor->setPixmap(sensorOffPixmap_);
        return;
    }

    items.led->setPixmap(ledPixmap(aggregated));
    items.sensor->setPixmap(alarmActive ? sensorActivePixmap_ : sensorSafePixmap_);
}

/** @brief 현재 세션에서 확인된 유효한 하드웨어 피드백인지 검사합니다. */
bool DeviceStatusMapOverlay::hasValidSignal(const ChannelStatusRecord& record) const {
    return signalAvailable_ && record.hasStatus && record.receivedInCurrentSession && record.status.hasConfirmedState &&
           record.status.feedbackHealth == DeviceFeedbackHealth::Confirmed;
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
