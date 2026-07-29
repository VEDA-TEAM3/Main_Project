#include "ui/panels/EventLogPanel.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "ui/EventLogTableModel.h"

namespace {
constexpr int eventTimeColumn = 0;
constexpr int eventAreaColumn = 1;
constexpr int eventObjectColumn = 2;
constexpr int eventRiskColumn = 3;
constexpr int eventActionColumn = 4;
constexpr int eventTimeColumnWidth = 58;
constexpr int eventAreaColumnWidth = 46;
constexpr int eventObjectColumnWidth = 76;
constexpr int eventRiskColumnWidth = 62;
constexpr int eventActionColumnWidth = 98;
constexpr auto eventLogScrollBarStyle = R"(
QScrollBar:vertical {
    background: transparent;
    border: none;
    width: 8px;
    margin: 36px 1px 4px 1px;
}
QScrollBar::handle:vertical {
    background: #356e91;
    border: 1px solid #519abe;
    border-radius: 4px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover {
    background: #49add7;
}
QScrollBar::add-line:vertical {
    background: transparent;
    border: none;
    height: 0;
    subcontrol-position: bottom;
    subcontrol-origin: margin;
}
QScrollBar::sub-line:vertical {
    background: transparent;
    border: none;
    height: 0;
    subcontrol-position: top;
    subcontrol-origin: margin;
}
QScrollBar::up-arrow:vertical,
QScrollBar::down-arrow:vertical {
    image: none;
    border: none;
    width: 0;
    height: 0;
}
QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: transparent;
    border: none;
}
)";
}  // namespace

/**
 * @brief         이벤트 로그 model과 view를 소유하는 패널을 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 위젯
 */
EventLogPanel::EventLogPanel(QWidget* parent) : QWidget(parent) { setupUi(); }

/**
 * @brief        새 이벤트 로그를 목록 최상단에 추가합니다.
 * @param entry  추가할 이벤트 로그
 */
void EventLogPanel::prependEntry(const EventLogEntry& entry) { prependEntries({entry}); }

/**
 * @brief          여러 이벤트를 추가하면서 사용자가 보고 있던 스크롤 위치를 보존합니다.
 * @param entries  추가할 이벤트 로그 목록
 */
void EventLogPanel::prependEntries(QVector<EventLogEntry> entries) {
    if (!model_ || !table_ || entries.isEmpty()) {
        return;
    }

    QScrollBar* scrollBar = table_->verticalScrollBar();
    const bool wasAtTop = scrollBar->value() == scrollBar->minimum();
    const int previousValue = scrollBar->value();
    const int insertedHeight = static_cast<int>(entries.size()) * table_->verticalHeader()->defaultSectionSize();

    model_->prependEntries(std::move(entries));

    if (wasAtTop) {
        scrollBar->setValue(scrollBar->minimum());
        return;
    }

    scrollBar->setValue(std::min(previousValue + insertedHeight, scrollBar->maximum()));
}

/** @brief 이벤트 로그와 스크롤 위치를 초기 상태로 되돌립니다. */
void EventLogPanel::clear() {
    if (!model_ || !table_) {
        return;
    }

    model_->clear();
    table_->verticalScrollBar()->setValue(table_->verticalScrollBar()->minimum());
}

/**
 * @brief       패널 크기가 바뀌면 현재 폭에 맞춰 열 너비를 다시 계산합니다.
 * @param event  Qt resize 이벤트
 */
void EventLogPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    resizeColumns();
    updateScrollHeaderCover();
}

/**
 * @brief   이벤트 로그 table view와 표시 속성을 구성합니다.
 */
void EventLogPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    model_ = new EventLogTableModel(this);
    table_ = new QTableView(this);

    table_->setObjectName(QStringLiteral("eventLogTable"));
    table_->setModel(model_);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(30);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsMovable(false);
    table_->horizontalHeader()->setSectionsClickable(false);
    table_->horizontalHeader()->setMinimumSectionSize(40);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    QScrollBar* verticalScrollBar = table_->verticalScrollBar();
    verticalScrollBar->setStyleSheet(QString::fromLatin1(eventLogScrollBarStyle));
    scrollHeaderCover_ = new QWidget(verticalScrollBar);
    scrollHeaderCover_->setObjectName(QStringLiteral("eventLogScrollHeaderCover"));
    scrollHeaderCover_->setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(verticalScrollBar, &QScrollBar::rangeChanged, this, [this](int, int) {
        QTimer::singleShot(0, this, [this]() {
            resizeColumns();
            updateScrollHeaderCover();
        });
    });
    layout->addWidget(table_);

    resizeColumns();
    QTimer::singleShot(0, this, [this]() {
        resizeColumns();
        updateScrollHeaderCover();
    });
}

/**
 * @brief   세로 스크롤바가 차지하는 헤더 오른쪽 빈 영역을 헤더 색상으로 마감합니다.
 */
void EventLogPanel::updateScrollHeaderCover() {
    if (!table_ || !scrollHeaderCover_) {
        return;
    }

    QScrollBar* scrollBar = table_->verticalScrollBar();
    const bool scrollBarVisible = scrollBar->isVisible() && scrollBar->maximum() > scrollBar->minimum();
    scrollHeaderCover_->setVisible(scrollBarVisible);

    if (!scrollBarVisible) {
        return;
    }

    QHeaderView* header = table_->horizontalHeader();
    scrollHeaderCover_->setGeometry(0, 0, scrollBar->width(), header->height());
    scrollHeaderCover_->raise();
}

/**
 * @brief   이벤트 로그의 다섯 열을 현재 viewport 폭에 맞춰 비례 조정합니다.
 */
void EventLogPanel::resizeColumns() {
    if (!table_) {
        return;
    }

    constexpr int totalBaseColumnWidth = eventTimeColumnWidth + eventAreaColumnWidth + eventObjectColumnWidth +
                                         eventRiskColumnWidth + eventActionColumnWidth;
    const int viewportWidth = table_->viewport()->width();

    if (viewportWidth <= 0) {
        return;
    }

    if (viewportWidth <= totalBaseColumnWidth) {
        const int timeWidth = std::max(48, viewportWidth * eventTimeColumnWidth / totalBaseColumnWidth);
        const int areaWidth = std::max(40, viewportWidth * eventAreaColumnWidth / totalBaseColumnWidth);
        const int objectWidth = std::max(64, viewportWidth * eventObjectColumnWidth / totalBaseColumnWidth);
        const int riskWidth = std::max(54, viewportWidth * eventRiskColumnWidth / totalBaseColumnWidth);
        const int actionWidth = std::max(72, viewportWidth - timeWidth - areaWidth - objectWidth - riskWidth);

        table_->setColumnWidth(eventTimeColumn, timeWidth);
        table_->setColumnWidth(eventAreaColumn, areaWidth);
        table_->setColumnWidth(eventObjectColumn, objectWidth);
        table_->setColumnWidth(eventRiskColumn, riskWidth);
        table_->setColumnWidth(eventActionColumn, actionWidth);
        return;
    }

    const int extraWidth = viewportWidth - totalBaseColumnWidth;
    const int timeWidth = eventTimeColumnWidth + (extraWidth * eventTimeColumnWidth / totalBaseColumnWidth);
    const int areaWidth = eventAreaColumnWidth + (extraWidth * eventAreaColumnWidth / totalBaseColumnWidth);
    const int objectWidth = eventObjectColumnWidth + (extraWidth * eventObjectColumnWidth / totalBaseColumnWidth);
    const int riskWidth = eventRiskColumnWidth + (extraWidth * eventRiskColumnWidth / totalBaseColumnWidth);
    const int actionWidth =
        std::max(eventActionColumnWidth, viewportWidth - timeWidth - areaWidth - objectWidth - riskWidth);

    table_->setColumnWidth(eventTimeColumn, timeWidth);
    table_->setColumnWidth(eventAreaColumn, areaWidth);
    table_->setColumnWidth(eventObjectColumn, objectWidth);
    table_->setColumnWidth(eventRiskColumn, riskWidth);
    table_->setColumnWidth(eventActionColumn, actionWidth);
}
