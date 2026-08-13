#pragma once

#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QRectF>
#include <QTimer>
#include <array>
#include <memory>

#include "model/DigitalTwinTypes.h"

/**
 * @brief 구역 하나의 채널 사분면을 위험 단계 색으로 채우고 서서히 나타내거나 지웁니다.
 *
 * @details 카메라 4대가 십자 통로 교차점에서 상/우/하/좌를 보므로 채널 경계는 구역 사각형의
 *          두 대각선이고, 채널 한 개는 삼각형 하나다. 색은 위험 단계가 바뀔 때만 갈아 끼우고
 *          투명도만 프레임마다 목표값으로 끌어 준다.
 */
class ChannelRiskOverlay final {
public:
    static constexpr int channelCount = 4;

    ChannelRiskOverlay();
    ~ChannelRiskOverlay();

    void attach(QGraphicsScene* scene, const QRectF& zoneRect);
    void setChannelRiskLevels(const std::array<DigitalTwinRiskLevel, channelCount>& riskLevels);
    void clear();

private:
    struct ChannelItem {
        std::unique_ptr<QGraphicsPathItem> item;
        DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
        qreal opacity = 0.0;
        qreal targetOpacity = 0.0;
    };

    void updateAnimations();

    QGraphicsScene* scene_ = nullptr;
    QTimer animationTimer_;
    std::array<ChannelItem, channelCount> channels_;
};
