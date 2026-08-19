#pragma once

#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QRectF>
#include <QTimer>
#include <memory>
#include <vector>

/**
 * @brief 위험 상태가 있는 동안 지도 바깥 테두리를 네온 빨강으로 깜박입니다.
 *
 * @details 지도 안에 별도 알림판을 띄우면 도면과 장치 상태 칩을 가린다. 테두리는 도면 요소가
 *          없는 자리라 무엇도 가리지 않으면서 시야 가장자리에서 바로 눈에 들어온다.
 *
 *          같은 사각형에 굵기와 밝기가 다른 획을 여러 겹 올려 네온관처럼 보이게 한다. 바깥
 *          겹일수록 굵고 흐려서 번지는 빛이 되고, 안쪽 심지는 거의 흰빛이다. 숨쉬는 것은
 *          바깥 광채뿐이고 **심지는 항상 밝게 남는다** — 관제 화면에서 경보가 파형의 골마다
 *          옅어지면 그 순간 위험 표시를 놓칠 수 있기 때문이다.
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

    std::vector<std::unique_ptr<QGraphicsRectItem>> layers_;
    QTimer animationTimer_;
    qint64 elapsedMsec_ = 0;
    /// 0.0~1.0 깜박임 위상. 위험이 풀리면 마지막 값에서 멈춘다
    qreal pulse_ = 0.0;
    /// 사라지는 동안 모든 겹에 함께 곱하는 계수
    qreal fade_ = 0.0;
    bool active_ = false;
};
