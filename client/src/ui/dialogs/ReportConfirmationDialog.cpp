#include "ui/dialogs/ReportConfirmationDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {
constexpr int dialogWidth = 460;
constexpr int dialogHeight = 230;
constexpr int minimumChannelNumber = 1;
constexpr int maximumChannelNumber = 4;
}  // namespace

/**
 * @brief        채널 신고 전 사용자의 최종 확인을 받는 모달 다이얼로그를 구성합니다.
 * @param parent 다이얼로그의 소유권과 모달 범위를 제공하는 부모 위젯
 */
ReportConfirmationDialog::ReportConfirmationDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("reportConfirmationDialog"));
    setWindowTitle(QStringLiteral("신고 확인"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowModality(Qt::WindowModal);
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(dialogWidth, dialogHeight);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("reportConfirmationPanel"));
    rootLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 22);
    panelLayout->setSpacing(18);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(10);

    auto* iconLabel = new QLabel(panel);
    iconLabel->setObjectName(QStringLiteral("reportConfirmationIcon"));
    iconLabel->setFixedSize(40, 40);
    const QPixmap reportIcon(QStringLiteral(":/icons/report_icon.png"));
    if (!reportIcon.isNull()) {
        iconLabel->setPixmap(reportIcon.scaled(QSize(36, 36), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    iconLabel->setContentsMargins(0, 4, 0, 0);
    iconLabel->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(iconLabel);

    auto* titleLabel = new QLabel(QStringLiteral("신고 확인"), panel);
    titleLabel->setObjectName(QStringLiteral("reportConfirmationTitle"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);

    auto* closeButton = new QPushButton(QStringLiteral("×"), panel);
    closeButton->setObjectName(QStringLiteral("reportDialogCloseButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip(QStringLiteral("닫기"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(closeButton);
    panelLayout->addLayout(headerLayout);

    messageLabel_ = new QLabel(panel);
    messageLabel_->setObjectName(QStringLiteral("reportConfirmationMessage"));
    messageLabel_->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(messageLabel_);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch(1);

    auto* cancelButton = new QPushButton(QStringLiteral("취소"), panel);
    cancelButton->setObjectName(QStringLiteral("reportDialogCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    auto* confirmButton = new QPushButton(QStringLiteral("확인"), panel);
    confirmButton->setObjectName(QStringLiteral("reportDialogConfirmButton"));
    confirmButton->setCursor(Qt::PointingHandCursor);
    confirmButton->setDefault(true);
    connect(confirmButton, &QPushButton::clicked, this, [this]() {
        const int confirmedChannelNumber = channelNumber_;
        accept();
        emit reportConfirmed(confirmedChannelNumber);
    });
    buttonLayout->addWidget(confirmButton);
    panelLayout->addLayout(buttonLayout);

    setChannelNumber(minimumChannelNumber);
}

/**
 * @brief               확인 문구에 표시할 채널 번호를 설정합니다.
 * @param channelNumber 1부터 4까지의 채널 번호
 */
void ReportConfirmationDialog::setChannelNumber(int channelNumber) {
    channelNumber_ = qBound(minimumChannelNumber, channelNumber, maximumChannelNumber);
    messageLabel_->setText(QStringLiteral("%1 채널을 신고하겠습니까?").arg(channelNumber_));
}
