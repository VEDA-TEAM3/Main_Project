#include "ui/dialogs/AreaSelectionDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QSize>
#include <QStringList>
#include <QVBoxLayout>

namespace {
constexpr int dialogWidth = 430;
constexpr int dialogHeight = 340;
constexpr int areaRowHeight = 50;
}  // namespace

/**
 * @brief        CCTV 모니터링 구역을 목록에서 선택하는 모달 다이얼로그를
 * 구성합니다.
 * @param parent 다이얼로그의 소유권과 모달 범위를 제공하는 부모 위젯
 */
AreaSelectionDialog::AreaSelectionDialog(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("areaSelectionDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setAlignment(Qt::AlignCenter);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("areaSelectionPanel"));
    panel->setFixedSize(dialogWidth, dialogHeight);
    rootLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(26, 22, 26, 24);
    panelLayout->setSpacing(16);

    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(QStringLiteral("모니터링 구역 선택"), panel);
    titleLabel->setObjectName(QStringLiteral("areaSelectionTitle"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);

    auto* closeButton = new QPushButton(QStringLiteral("×"), panel);
    closeButton->setObjectName(QStringLiteral("areaSelectionCloseButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setToolTip(QStringLiteral("닫기"));
    connect(closeButton, &QPushButton::clicked, this, &QWidget::hide);
    headerLayout->addWidget(closeButton);
    panelLayout->addLayout(headerLayout);

    auto* descriptionLabel = new QLabel(QStringLiteral("화면에 출력할 주차 구역을 선택하세요."), panel);
    descriptionLabel->setObjectName(QStringLiteral("areaSelectionDescription"));
    panelLayout->addWidget(descriptionLabel);

    areaList_ = new QListWidget(panel);
    areaList_->setObjectName(QStringLiteral("areaSelectionList"));
    areaList_->setCursor(Qt::PointingHandCursor);
    areaList_->setFocusPolicy(Qt::NoFocus);
    areaList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(areaList_, &QListWidget::itemDoubleClicked, this, [this]() { confirmSelection(); });
    panelLayout->addWidget(areaList_, 1);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch(1);

    auto* cancelButton = new QPushButton(QStringLiteral("취소"), panel);
    cancelButton->setObjectName(QStringLiteral("areaSelectionCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setFocusPolicy(Qt::NoFocus);
    connect(cancelButton, &QPushButton::clicked, this, &QWidget::hide);
    buttonLayout->addWidget(cancelButton);

    auto* selectButton = new QPushButton(QStringLiteral("선택"), panel);
    selectButton->setObjectName(QStringLiteral("areaSelectionConfirmButton"));
    selectButton->setCursor(Qt::PointingHandCursor);
    selectButton->setFocusPolicy(Qt::NoFocus);
    connect(selectButton, &QPushButton::clicked, this, &AreaSelectionDialog::confirmSelection);
    buttonLayout->addWidget(selectButton);
    panelLayout->addLayout(buttonLayout);

    hide();
}

/**
 * @brief                  표시할 구역 목록과 현재 구역을 갱신합니다.
 * @param areaNames        설정 파일에서 읽은 구역 이름 목록
 * @param currentAreaIndex 현재 표시 중인 구역 인덱스
 */
void AreaSelectionDialog::setAreas(const QStringList& areaNames, int currentAreaIndex) {
    areaList_->clear();
    for (qsizetype areaIndex = 0; areaIndex < areaNames.size(); ++areaIndex) {
        auto* item = new QListWidgetItem(areaNames[areaIndex], areaList_);
        item->setData(Qt::UserRole, areaIndex);
        item->setSizeHint(QSize(0, areaRowHeight));
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    }

    setCurrentAreaIndex(currentAreaIndex);
}

/**
 * @brief                  이미 생성된 목록에서 현재 구역 선택만 갱신합니다.
 * @param currentAreaIndex 현재 표시 중인 구역 인덱스
 */
void AreaSelectionDialog::setCurrentAreaIndex(int currentAreaIndex) {
    if (areaList_->count() <= 0) {
        return;
    }

    areaList_->setCurrentRow(qBound(0, currentAreaIndex, areaList_->count() - 1));
}

/** @brief 선택한 구역을 메인 화면에 전달하고 오버레이를 닫습니다. */
void AreaSelectionDialog::confirmSelection() {
    const QListWidgetItem* item = areaList_->currentItem();
    if (!item) {
        return;
    }

    emit areaSelected(item->data(Qt::UserRole).toInt());
    hide();
}

/**
 * @brief       오버레이를 부모 윈도우 전체에 맞추고 가장 위에 표시합니다.
 * @param event Qt 표시 이벤트
 */
void AreaSelectionDialog::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
    }
    raise();
    setFocus(Qt::PopupFocusReason);
}
