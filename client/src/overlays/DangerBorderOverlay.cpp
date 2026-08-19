#include "overlays/DangerBorderOverlay.h"

#include <QBrush>
#include <QColor>
#include <QObject>
#include <QPen>
#include <QtGlobal>
#include <iterator>
#include <utility>

namespace {
constexpr int overlayFrameIntervalMsec = 33;
/// 한 번 차올랐다 잦아드는 데 걸리는 시간. 짧을수록 다급해 보인다
constexpr qint64 blinkPeriodMsec = 1500;
/// 한 주기에서 차오르는 데 쓰는 비율. 나머지는 잦아드는 데 쓴다
constexpr double attackRatio = 0.32;
/// 짧게 스치는 위험도 알아볼 수 있도록 최소한 이만큼은 깜박인다
constexpr qint64 minimumVisibleMsec = 2000;
/// 위험이 풀린 뒤 사라지는 데 걸리는 시간
constexpr qreal fadeOutStepPerFrame = static_cast<qreal>(overlayFrameIntervalMsec) / 380.0;
/// 객체(4.0)와 상태 칩(5.2)보다 위에 그린다
constexpr qreal borderZValue = 20.0;

/** @brief 네온 획 한 겹의 굵기와 밝기 범위 */
struct NeonStroke {
    qreal penWidth;
    qreal minimumOpacity;
    qreal maximumOpacity;
    const char* color;
};

// 바깥에서 안으로. 굵고 흐린 겹이 번지는 빛이 되고 안쪽으로 갈수록 좁고 진해진다.
//
// scene 사각형이 테두리 바깥으로 6만 여유를 두므로(DigitalTwinMapSceneBuilder) 획은 중심선
// 기준 절반만 밖으로 나간다 — 가장 굵은 10짜리가 5까지다. 이 값을 12 이상으로 올리면 광채가
// scene 밖으로 잘린다.
//
// 흰빛 심지는 쓰지 않는다. 흰색은 배경과의 대비가 너무 커서 도면 위에서 혼자 튀고, 붉은
// 광채와 색이 끊겨 네온이 아니라 형광등처럼 보인다. 심지도 붉은 계열로 두면 겹 사이의 색이
// 이어져 한 덩어리의 빛으로 읽힌다.
//
// 최고 밝기는 낮추되 심지의 최소 밝기(0.72)는 지킨다. 숨쉬는 것은 바깥 광채뿐이고 얇은 선은
// 항상 또렷해야, 파형의 골에서도 경보를 놓치지 않는다.
constexpr NeonStroke neonStrokes[] = {
    {10.0, 0.04, 0.20, "#ff2f3d"},
    {6.0, 0.10, 0.34, "#ff2f3d"},
    {3.0, 0.24, 0.56, "#ff3b48"},
    {1.2, 0.72, 0.90, "#ff6169"},
};
}  // namespace

/** @brief 깜박임 갱신 타이머를 준비합니다. 타이머는 이 객체를 만든 UI 스레드에서 돕니다. */
DangerBorderOverlay::DangerBorderOverlay() {
    animationTimer_.setInterval(overlayFrameIntervalMsec);
    animationTimer_.setTimerType(Qt::PreciseTimer);

    QObject::connect(&animationTimer_, &QTimer::timeout, [this]() { updateAnimation(); });
}

DangerBorderOverlay::~DangerBorderOverlay() { clear(); }

/**
 * @brief             지도 테두리 사각형을 scene에 올립니다.
 * @param scene       테두리를 올릴 scene
 * @param borderRect  지도 전체를 감싸는 사각형
 */
void DangerBorderOverlay::attach(QGraphicsScene* scene, const QRectF& borderRect) {
    clear();

    if (!scene || borderRect.isEmpty()) {
        return;
    }

    layers_.reserve(std::size(neonStrokes));
    qreal layerZValue = borderZValue;
    for (const NeonStroke& stroke : neonStrokes) {
        QPen borderPen(QColor(QLatin1String(stroke.color)), stroke.penWidth);
        borderPen.setJoinStyle(Qt::MiterJoin);

        auto item = std::make_unique<QGraphicsRectItem>(borderRect);
        item->setPen(borderPen);
        item->setBrush(QBrush(Qt::NoBrush));
        // 굵은 겹부터 올려야 좁고 밝은 심지가 그 위에 남는다
        item->setZValue(layerZValue);
        layerZValue += 0.01;
        item->setOpacity(0.0);
        item->setVisible(false);
        // 구역 클릭은 뷰가 좌표로 판정하므로 이 아이템이 입력을 가로채면 안 된다
        item->setAcceptedMouseButtons(Qt::NoButton);
        scene->addItem(item.get());
        layers_.push_back(std::move(item));
    }
}

/**
 * @brief         위험 상태 유무를 반영합니다.
 * @param active  하나 이상의 위험 상태가 존재하면 true
 */
void DangerBorderOverlay::setActive(bool active) {
    if (active_ == active) {
        return;
    }

    active_ = active;
    if (active_) {
        elapsedMsec_ = 0;
    }

    if (!layers_.empty() && !animationTimer_.isActive()) {
        animationTimer_.start();
    }
}

/** @brief scene에 올린 테두리를 떼어 냅니다. */
void DangerBorderOverlay::clear() {
    animationTimer_.stop();

    for (const std::unique_ptr<QGraphicsRectItem>& item : layers_) {
        if (item && item->scene()) {
            item->scene()->removeItem(item.get());
        }
    }

    layers_.clear();
    elapsedMsec_ = 0;
    pulse_ = 0.0;
    fade_ = 0.0;
    active_ = false;
}

/** @brief 테두리 밝기를 한 프레임만큼 갱신합니다. */
void DangerBorderOverlay::updateAnimation() {
    if (layers_.empty()) {
        animationTimer_.stop();
        return;
    }

    elapsedMsec_ += overlayFrameIntervalMsec;

    // 위험이 풀려도 최소 표시 시간까지는 계속 깜박인다
    const bool blinking = active_ || elapsedMsec_ < minimumVisibleMsec;
    if (blinking) {
        // 빠르게 차오르고 천천히 잦아드는 비대칭 파형이다. 대칭 사인파는 "숨쉰다"로 읽히고
        // 이쪽은 "경보"로 읽힌다. 삼각파를 smoothstep으로 눕혀 꺾이는 지점을 없앤다.
        // 삼각함수를 쓰지 않는 이유는 이 MinGW 구성에서 우리 TU가 libm을 직접 부르면
        // 실행 즉시 pseudo relocation으로 죽기 때문이다(예전에 std::cos로 겪었다)
        const double phase = static_cast<double>(elapsedMsec_ % blinkPeriodMsec) / blinkPeriodMsec;
        const double ramp =
            phase < attackRatio ? phase / attackRatio : 1.0 - (phase - attackRatio) / (1.0 - attackRatio);
        pulse_ = ramp * ramp * (3.0 - 2.0 * ramp);
        fade_ = 1.0;
    } else {
        // 위상은 마지막 값에서 멈추고 전체가 함께 사라진다
        fade_ = qMax(0.0, fade_ - fadeOutStepPerFrame);
    }

    for (size_t index = 0; index < layers_.size(); ++index) {
        const NeonStroke& stroke = neonStrokes[index];
        const qreal strokeOpacity =
            (stroke.minimumOpacity + (stroke.maximumOpacity - stroke.minimumOpacity) * pulse_) * fade_;
        layers_[index]->setOpacity(strokeOpacity);
        layers_[index]->setVisible(strokeOpacity > 0.0);
    }

    if (!blinking && fade_ <= 0.0) {
        animationTimer_.stop();
    }
}
