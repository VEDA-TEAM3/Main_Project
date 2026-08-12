#include "ui/panels/EventLogPanel.h"

#include <QQuickWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <utility>

#include "ui/EventLogTableModel.h"
#include "ui/SharedQmlEngine.h"

/**
 * @brief         이벤트 로그 model과 QML 표를 소유하는 패널을 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 위젯
 */
EventLogPanel::EventLogPanel(QWidget* parent) : QWidget(parent) { setupUi(); }

/**
 * @brief        새 이벤트 로그를 목록 최상단에 추가합니다.
 * @param entry  추가할 이벤트 로그
 */
void EventLogPanel::prependEntry(const EventLogEntry& entry) { prependEntries({entry}); }

/**
 * @brief          여러 이벤트를 한 번의 model 변경으로 추가합니다.
 * @param entries  추가할 이벤트 로그 목록
 */
void EventLogPanel::prependEntries(QVector<EventLogEntry> entries) {
    if (model_) {
        model_->prependEntries(std::move(entries));
    }
}

/** @brief 현재 표시 중인 이벤트 로그를 모두 지웁니다. */
void EventLogPanel::clear() {
    if (model_) {
        model_->clear();
    }
}

/**
 * @brief   이벤트 로그 QML 표를 패널에 배치합니다.
 */
void EventLogPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    model_ = new EventLogTableModel(this);

    QQuickWidget* view =
        createQmlPanelView(QStringLiteral("DataTable.qml"), this,
                           {{QStringLiteral("tableModel"), QVariant::fromValue<QObject*>(model_)},
                            {QStringLiteral("columnWeights"), QVariantList{0.135, 0.150, 0.135, 0.197, 0.150, 0.233}}});

    if (view) {
        layout->addWidget(view);
    }
}
