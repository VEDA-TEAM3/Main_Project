#include "ui/panels/ObjectListPanel.h"

#include <QQuickWidget>
#include <QVBoxLayout>
#include <utility>

#include "ui/DigitalTwinObjectTableModel.h"
#include "ui/SharedQmlEngine.h"

/**
 * @brief         실시간 객체 목록의 model과 QML 표를 소유하는 패널을 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 위젯
 */
ObjectListPanel::ObjectListPanel(QWidget* parent) : QWidget(parent) { setupUi(); }

/**
 * @brief          최신 디지털 트윈 객체 목록을 패널 model에 반영합니다.
 * @param objects  현재 맵에 존재하는 객체 목록
 */
void ObjectListPanel::setObjects(QVector<DigitalTwinObject> objects) {
    if (model_) {
        model_->updateObjects(std::move(objects));
    }
}

/**
 * @brief   객체 목록 QML 표를 패널에 배치합니다.
 */
void ObjectListPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    model_ = new DigitalTwinObjectTableModel(this);

    QQuickWidget* view =
        createQmlPanelView(QStringLiteral("DataTable.qml"), this,
                           {{QStringLiteral("tableModel"), QVariant::fromValue<QObject*>(model_)},
                            {QStringLiteral("columnWeights"), QVariantList{0.16, 0.23, 0.26, 0.19, 0.16}}});

    if (view) {
        layout->addWidget(view);
    }
}
