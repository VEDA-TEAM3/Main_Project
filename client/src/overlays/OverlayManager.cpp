#include "overlays/OverlayManager.h"

#include <QObject>
#include <memory>

#include "overlays/RadarPulseItem.h"

namespace {
constexpr int overlayFrameIntervalMsec = 33;
constexpr int maxActivePulseItemCount = 24;
}  // namespace

/**
 * @brief   오버레이 갱신 타이머를 준비합니다.
 * @details 타이머는 OverlayManager가 생성된 스레드에서 동작하며, 현재 구조에서는 UI 스레드에서만 사용합니다.
 */
OverlayManager::OverlayManager() {
    animationTimer_.setInterval(overlayFrameIntervalMsec);
    animationTimer_.setTimerType(Qt::PreciseTimer);

    QObject::connect(&animationTimer_, &QTimer::timeout, [this]() { updateAnimations(); });
}

/**
 * @brief   남은 오버레이 아이템을 모두 제거합니다.
 */
OverlayManager::~OverlayManager() { clear(); }

/**
 * @brief       오버레이를 표시할 scene을 지정합니다.
 * @param scene  오버레이 아이템을 추가할 QGraphicsScene
 */
void OverlayManager::setScene(QGraphicsScene* scene) {
    if (scene_ == scene) {
        return;
    }

    clear();
    scene_ = scene;
}

/**
 * @brief            감지 위치에 레이더 펄스 오버레이를 추가합니다.
 * @param scenePosition  scene 좌표계 기준 발생 위치
 * @param riskLevel      표시할 주의/위험 단계
 */
void OverlayManager::showRiskPulse(const QPointF& scenePosition, DigitalTwinRiskLevel riskLevel) {
    if (!scene_ || riskLevel == DigitalTwinRiskLevel::Normal) {
        return;
    }

    if (activePulseItems_.size() >= maxActivePulseItemCount) {
        removePulseAt(0);
    }

    auto pulseItem = std::make_shared<RadarPulseItem>(riskLevel);
    pulseItem->setPos(scenePosition);
    pulseItem->setElapsedMsec(0);

    scene_->addItem(pulseItem.get());
    activePulseItems_.append({pulseItem, 0});

    if (!animationTimer_.isActive()) {
        animationTimer_.start();
    }
}

/**
 * @brief   현재 scene에 살아있는 오버레이 아이템을 모두 제거합니다.
 */
void OverlayManager::clear() {
    animationTimer_.stop();

    while (!activePulseItems_.isEmpty()) {
        removePulseAt(activePulseItems_.size() - 1);
    }
}

/**
 * @brief   진행 중인 펄스 애니메이션을 한 프레임만큼 갱신합니다.
 */
void OverlayManager::updateAnimations() {
    for (qsizetype index = activePulseItems_.size() - 1; index >= 0; --index) {
        auto& activePulseItem = activePulseItems_[index];

        if (!activePulseItem.item) {
            activePulseItems_.removeAt(index);
            continue;
        }

        activePulseItem.elapsedMsec += overlayFrameIntervalMsec;
        activePulseItem.item->setElapsedMsec(activePulseItem.elapsedMsec);

        if (activePulseItem.item->isFinished()) {
            removePulseAt(index);
        }
    }

    if (activePulseItems_.isEmpty()) {
        animationTimer_.stop();
    }
}

/**
 * @brief       특정 펄스 아이템을 scene에서 분리한 뒤 삭제합니다.
 * @param index  제거할 active pulse 인덱스
 */
void OverlayManager::removePulseAt(qsizetype index) {
    if (index < 0 || index >= activePulseItems_.size()) {
        return;
    }

    std::shared_ptr<RadarPulseItem> item = activePulseItems_[index].item;
    activePulseItems_.removeAt(index);

    if (!item) {
        return;
    }

    if (item->scene()) {
        item->scene()->removeItem(item.get());
    }
}
