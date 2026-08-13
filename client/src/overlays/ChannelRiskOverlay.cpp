#include "overlays/ChannelRiskOverlay.h"

#include <QBrush>
#include <QColor>
#include <QObject>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QtGlobal>

namespace {
constexpr int overlayFrameIntervalMsec = 33;
/// 한 프레임에 움직일 투명도. 약 220ms에 완전히 나타나고 같은 시간에 사라진다
constexpr qreal fadeStepPerFrame = static_cast<qreal>(overlayFrameIntervalMsec) / 220.0;
/// 도면 선과 객체 아이콘이 계속 보여야 하므로 채우기는 옅게 깐다
constexpr qreal warningTargetOpacity = 0.30;
constexpr qreal dangerTargetOpacity = 0.40;
/// 주차 구획(1.0~1.3)보다 위, 채널 경계선(3.2)과 객체(4.0)보다 아래
constexpr qreal channelRiskZValue = 2.6;

QColor riskColor(DigitalTwinRiskLevel riskLevel) {
    if (riskLevel == DigitalTwinRiskLevel::Danger) {
        return QColor(QStringLiteral("#ff2f3d"));
    }

    return QColor(QStringLiteral("#ffd43b"));
}

qreal targetOpacityForRiskLevel(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Danger:
            return dangerTargetOpacity;
        case DigitalTwinRiskLevel::Warning:
            return warningTargetOpacity;
        case DigitalTwinRiskLevel::Normal:
            return 0.0;
    }

    return 0.0;
}

/**
 * @brief               구역 사각형을 두 대각선으로 자른 삼각형 하나를 돌려줍니다.
 * @param zoneRect      구역 사각형
 * @param channelIndex  0=위(CH01), 1=오른쪽(CH02), 2=아래(CH03), 3=왼쪽(CH04)
 * @return              도면의 CH 이름표와 같은 자리를 덮는 삼각형 path
 */
QPainterPath channelTrianglePath(const QRectF& zoneRect, int channelIndex) {
    const std::array<QPointF, 4> corners = {zoneRect.topLeft(), zoneRect.topRight(), zoneRect.bottomRight(),
                                            zoneRect.bottomLeft()};

    QPainterPath path;
    path.moveTo(corners[channelIndex]);
    path.lineTo(corners[(channelIndex + 1) % 4]);
    path.lineTo(zoneRect.center());
    path.closeSubpath();
    return path;
}
}  // namespace

/** @brief 페이드 갱신 타이머를 준비합니다. 타이머는 이 객체를 만든 UI 스레드에서 돕니다. */
ChannelRiskOverlay::ChannelRiskOverlay() {
    animationTimer_.setInterval(overlayFrameIntervalMsec);
    animationTimer_.setTimerType(Qt::PreciseTimer);

    QObject::connect(&animationTimer_, &QTimer::timeout, [this]() { updateAnimations(); });
}

ChannelRiskOverlay::~ChannelRiskOverlay() { clear(); }

/**
 * @brief           구역 사각형에 맞춘 채널 삼각형 네 개를 scene에 올립니다.
 * @param scene     삼각형을 올릴 scene
 * @param zoneRect  구역 사각형
 */
void ChannelRiskOverlay::attach(QGraphicsScene* scene, const QRectF& zoneRect) {
    clear();

    if (!scene || zoneRect.isEmpty()) {
        return;
    }
    scene_ = scene;

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        ChannelItem& channel = channels_[channelIndex];
        channel.item = std::make_unique<QGraphicsPathItem>(channelTrianglePath(zoneRect, channelIndex));
        channel.item->setPen(QPen(Qt::NoPen));
        channel.item->setBrush(QBrush(riskColor(DigitalTwinRiskLevel::Warning)));
        channel.item->setZValue(channelRiskZValue);
        channel.item->setOpacity(0.0);
        channel.item->setVisible(false);
        // 구역 클릭은 뷰가 좌표로 판정하므로 이 아이템이 입력을 가로채면 안 된다
        channel.item->setAcceptedMouseButtons(Qt::NoButton);
        scene_->addItem(channel.item.get());
    }
}

/**
 * @brief             채널별 위험 단계를 반영합니다.
 * @param riskLevels  이 구역의 CH01~CH04 위험 단계
 */
void ChannelRiskOverlay::setChannelRiskLevels(const std::array<DigitalTwinRiskLevel, channelCount>& riskLevels) {
    bool changed = false;

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        ChannelItem& channel = channels_[channelIndex];
        if (!channel.item || channel.riskLevel == riskLevels[channelIndex]) {
            continue;
        }

        channel.riskLevel = riskLevels[channelIndex];
        channel.targetOpacity = targetOpacityForRiskLevel(channel.riskLevel);
        if (channel.riskLevel != DigitalTwinRiskLevel::Normal) {
            channel.item->setBrush(QBrush(riskColor(channel.riskLevel)));
        }
        changed = true;
    }

    if (changed && !animationTimer_.isActive()) {
        animationTimer_.start();
    }
}

/** @brief scene에 올린 삼각형을 모두 떼어 냅니다. */
void ChannelRiskOverlay::clear() {
    animationTimer_.stop();

    for (ChannelItem& channel : channels_) {
        if (channel.item && channel.item->scene()) {
            channel.item->scene()->removeItem(channel.item.get());
        }

        channel.item.reset();
        channel.riskLevel = DigitalTwinRiskLevel::Normal;
        channel.opacity = 0.0;
        channel.targetOpacity = 0.0;
    }

    scene_ = nullptr;
}

/** @brief 각 삼각형의 투명도를 목표값 쪽으로 한 프레임만큼 옮깁니다. */
void ChannelRiskOverlay::updateAnimations() {
    bool animating = false;

    for (ChannelItem& channel : channels_) {
        if (!channel.item) {
            continue;
        }

        const qreal difference = channel.targetOpacity - channel.opacity;
        if (qAbs(difference) <= fadeStepPerFrame) {
            channel.opacity = channel.targetOpacity;
        } else {
            channel.opacity += difference > 0.0 ? fadeStepPerFrame : -fadeStepPerFrame;
            animating = true;
        }

        channel.item->setOpacity(channel.opacity);
        channel.item->setVisible(channel.opacity > 0.0);
    }

    if (!animating) {
        animationTimer_.stop();
    }
}
