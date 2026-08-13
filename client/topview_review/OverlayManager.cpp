#include "overlays/OverlayManager.h"

#include <QObject>
#include <cmath>
#include <memory>

#include "overlays/RadarPulseItem.h"

namespace {
constexpr int overlayFrameIntervalMsec = 33;
// 파동 하나가 프레임당 약 2ms를 먹는다(안티에일리어싱 원 스트로크 8회). 30fps 예산이 33ms인데
// GUI 스레드는 영상 위젯과 표까지 같이 그리므로, 동시 개수를 4로 묶어 8ms 안쪽으로 유지한다
constexpr int maxActivePulseItemCount = 4;
// 객체가 몰리면 거의 같은 자리에 파동이 여러 개 겹쳐 뜬다. 보이는 그림은 하나와 다를 게 없으므로
// 이 거리 안에 같은 단계 이상의 파동이 이미 퍼지는 중이면 새로 만들지 않는다.
// 파동 반지름이 180px까지 커지므로 이 정도는 사실상 같은 원이다(실데이터 10x10m 기준 약 1.3m)
constexpr double pulseMergeDistance = 50.0;
// 이미 절반 넘게 퍼진 파동은 곧 사라지므로 겹침으로 보지 않는다
constexpr qreal pulseMergeMaximumProgress = 0.5;
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

    if (isCoveredByActivePulse(scenePosition, riskLevel)) {
        return;
    }

    if (activePulseItems_.size() >= maxActivePulseItemCount) {
        removePulseAt(0);
    }

    auto pulseItem = std::make_shared<RadarPulseItem>(riskLevel);
    pulseItem->setPos(scenePosition);
    pulseItem->setElapsedMsec(0);

    scene_->addItem(pulseItem.get());
    activePulseItems_.append({pulseItem, 0, scenePosition, riskLevel});

    if (!animationTimer_.isActive()) {
        animationTimer_.start();
    }
}

/**
 * @brief                 같은 자리에서 이미 퍼지고 있는 파동이 있는지 확인합니다.
 * @param scenePosition   새로 띄우려는 위치
 * @param riskLevel       새로 띄우려는 단계
 * @return                덮이면 true
 *
 * @details 객체가 셋 이상 몰리면 쌍마다 파동이 생겨 거의 같은 자리에 여러 겹이 쌓인다. 겹친 그림은
 *          한 겹과 구분되지 않으면서 비용만 배로 든다. 단, 더 높은 단계는 색이 다르므로 항상 띄운다.
 */
bool OverlayManager::isCoveredByActivePulse(const QPointF& scenePosition, DigitalTwinRiskLevel riskLevel) const {
    for (const ActivePulseItem& activePulseItem : activePulseItems_) {
        if (!activePulseItem.item || activePulseItem.item->progress() > pulseMergeMaximumProgress) {
            continue;
        }

        if (static_cast<int>(activePulseItem.riskLevel) < static_cast<int>(riskLevel)) {
            continue;
        }

        const QPointF offset = activePulseItem.position - scenePosition;
        if (std::hypot(offset.x(), offset.y()) <= pulseMergeDistance) {
            return true;
        }
    }

    return false;
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
