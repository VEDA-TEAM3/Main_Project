#include "ui/dialogs/InformationDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int dialogWidth = 480;
constexpr int dialogHeight = 210;
}  // namespace

/**
 * @brief        작업 완료 결과를 현재 테마로 안내하는 재사용 가능한 모달 다이얼로그를 구성합니다.
 * @param parent 다이얼로그의 소유권과 모달 범위를 제공하는 부모 위젯
 */
InformationDialog::InformationDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("informationDialog"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowModality(Qt::WindowModal);
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(dialogWidth, dialogHeight);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("informationPanel"));
    rootLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 22);
    panelLayout->setSpacing(18);

    titleLabel_ = new QLabel(panel);
    titleLabel_->setObjectName(QStringLiteral("informationTitle"));
    panelLayout->addWidget(titleLabel_);

    messageLabel_ = new QLabel(panel);
    messageLabel_->setObjectName(QStringLiteral("informationMessage"));
    messageLabel_->setAlignment(Qt::AlignCenter);
    messageLabel_->setWordWrap(true);
    panelLayout->addWidget(messageLabel_, 1);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(1);

    auto* confirmButton = new QPushButton(QStringLiteral("확인"), panel);
    confirmButton->setObjectName(QStringLiteral("informationConfirmButton"));
    confirmButton->setCursor(Qt::PointingHandCursor);
    confirmButton->setDefault(true);
    connect(confirmButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(confirmButton);
    panelLayout->addLayout(buttonLayout);
}

/**
 * @brief         안내 팝업에 표시할 제목과 본문을 갱신합니다.
 * @param title   팝업 제목
 * @param message 작업 완료 안내 문구
 */
void InformationDialog::setContent(const QString& title, const QString& message) {
    setWindowTitle(title);
    titleLabel_->setText(title);
    messageLabel_->setText(message);
}
