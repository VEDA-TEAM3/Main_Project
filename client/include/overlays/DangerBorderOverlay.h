#pragma once

#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QRectF>
#include <QTimer>
#include <memory>

/**
 * @brief 위험 상태가 있는 동안 지도 바깥 테두리를 빨간색으로 깜박입니다.
 *
 * @details 지도 안에 별도 알림판을 띄우면 도면과 장치 상태 칩을 가린다. 테두리는 도면 요소가
 *          없는 자리라 무엇도 가리지 않으면서 시야 가장자리에서 바로 눈에 들어온다.
 */
class DangerBorderOverlay final {
public:
    DangerBorderOverlay();
    ~DangerBorderOverlay();

    void attach(QGraphicsScene* scene, const QRectF& borderRect);
    void setActive(bool active);
    void clear();

private:
    void updateAnimation();

    std::unique_ptr<QGraphicsRectItem> item_;
    QTimer animationTimer_;
    qint64 elapsedMsec_ = 0;
    qreal opacity_ = 0.0;
    bool active_ = false;
};
