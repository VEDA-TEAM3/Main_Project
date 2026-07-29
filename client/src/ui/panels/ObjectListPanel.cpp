#include "ui/panels/ObjectListPanel.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QFontMetrics>
#include <QHeaderView>
#include <QIcon>
#include <QPainter>
#include <QResizeEvent>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "ui/DigitalTwinObjectTableModel.h"

namespace {
constexpr int objectIdColumn = 0;
constexpr int objectTypeColumn = 1;
constexpr int objectPositionColumn = 2;
constexpr int objectAreaColumn = 3;
constexpr int objectIdColumnWidth = 64;
constexpr int objectTypeColumnWidth = 98;
constexpr int objectPositionColumnWidth = 104;
constexpr int objectAreaColumnWidth = 74;
constexpr int objectTypeIconTextSpacing = 6;

class ObjectTypeItemDelegate final : public QStyledItemDelegate {
public:
    /**
     * @brief        객체 유형 열 전용 delegate를 생성합니다.
     * @param parent  Qt 객체 소유권을 연결할 부모 객체
     */
    explicit ObjectTypeItemDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    /**
     * @brief         객체 아이콘과 유형 문자열을 한 묶음으로 중앙 정렬해 그립니다.
     * @param painter  셀을 그릴 painter
     * @param option   셀 표시 옵션
     * @param index    표시할 model index
     */
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem backgroundOption(option);
        initStyleOption(&backgroundOption, index);
        backgroundOption.text.clear();
        backgroundOption.icon = QIcon();

        const QWidget* widget = backgroundOption.widget;
        if (widget) {
            widget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &backgroundOption, painter, widget);
        }

        const QString text = index.data(Qt::DisplayRole).toString();
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QSize iconSize = option.decorationSize.isValid() ? option.decorationSize : QSize(18, 18);
        const QFontMetrics fontMetrics(option.font);
        const int textWidth = fontMetrics.horizontalAdvance(text);
        const int groupWidth = iconSize.width() + objectTypeIconTextSpacing + textWidth;
        const int groupLeft = option.rect.left() + (option.rect.width() - groupWidth) / 2;
        const int iconTop = option.rect.top() + (option.rect.height() - iconSize.height()) / 2;
        const QRect iconRect(groupLeft, iconTop, iconSize.width(), iconSize.height());
        const QRect textRect(groupLeft + iconSize.width() + objectTypeIconTextSpacing, option.rect.top(), textWidth,
                             option.rect.height());

        painter->save();
        painter->setPen(qvariant_cast<QBrush>(index.data(Qt::ForegroundRole)).color());
        icon.paint(painter, iconRect, Qt::AlignCenter);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
        painter->restore();
    }
};
}  // namespace

/**
 * @brief         실시간 객체 목록의 model, view, delegate를 소유하는 패널을 생성합니다.
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
 * @brief       패널 크기가 바뀌면 현재 폭에 맞춰 열 너비를 다시 계산합니다.
 * @param event  Qt resize 이벤트
 */
void ObjectListPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    resizeColumns();
}

/**
 * @brief   객체 목록 table view와 표시 속성을 구성합니다.
 */
void ObjectListPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    model_ = new DigitalTwinObjectTableModel(this);
    table_ = new QTableView(this);
    objectTypeDelegate_ = new ObjectTypeItemDelegate(table_);

    table_->setObjectName(QStringLiteral("objectListTable"));
    table_->setModel(model_);
    table_->setItemDelegateForColumn(objectTypeColumn, objectTypeDelegate_);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->setIconSize(QSize(18, 18));
    table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    layout->addWidget(table_);

    resizeColumns();
    QTimer::singleShot(0, this, &ObjectListPanel::resizeColumns);
}

/**
 * @brief   객체 목록의 네 열을 현재 viewport 폭에 맞춰 비례 조정합니다.
 */
void ObjectListPanel::resizeColumns() {
    if (!table_) {
        return;
    }

    constexpr int totalBaseColumnWidth =
        objectIdColumnWidth + objectTypeColumnWidth + objectPositionColumnWidth + objectAreaColumnWidth;
    const int viewportWidth = table_->viewport()->width();

    if (viewportWidth <= 0) {
        return;
    }

    if (viewportWidth <= totalBaseColumnWidth) {
        table_->setColumnWidth(objectIdColumn, objectIdColumnWidth);
        table_->setColumnWidth(objectTypeColumn, objectTypeColumnWidth);
        table_->setColumnWidth(objectPositionColumn, objectPositionColumnWidth);
        table_->setColumnWidth(objectAreaColumn, objectAreaColumnWidth);
        return;
    }

    const int extraWidth = viewportWidth - totalBaseColumnWidth;
    const int idWidth = objectIdColumnWidth + (extraWidth * objectIdColumnWidth / totalBaseColumnWidth);
    const int typeWidth = objectTypeColumnWidth + (extraWidth * objectTypeColumnWidth / totalBaseColumnWidth);
    const int positionWidth =
        objectPositionColumnWidth + (extraWidth * objectPositionColumnWidth / totalBaseColumnWidth);
    const int areaWidth = std::max(objectAreaColumnWidth, viewportWidth - idWidth - typeWidth - positionWidth);

    table_->setColumnWidth(objectIdColumn, idWidth);
    table_->setColumnWidth(objectTypeColumn, typeWidth);
    table_->setColumnWidth(objectPositionColumn, positionWidth);
    table_->setColumnWidth(objectAreaColumn, areaWidth);
}
