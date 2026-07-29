#include "ui/dialogs/ReportSuccessDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {
constexpr int dialogWidth = 460;
constexpr int dialogHeight = 215;
constexpr int minimumChannelNumber = 1;
constexpr int maximumChannelNumber = 4;
}  // namespace

/**
 * @brief        안전 센터 신고 완료 결과를 안내하는 모달 다이얼로그를 구성합니다.
 * @param parent 다이얼로그의 소유권과 모달 범위를 제공하는 부모 위젯
 */
ReportSuccessDialog::ReportSuccessDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("reportSuccessDialog"));
    setWindowTitle(QStringLiteral("신고 완료"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowModality(Qt::WindowModal);
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(dialogWidth, dialogHeight);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("reportSuccessPanel"));
    rootLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 22);
    panelLayout->setSpacing(18);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(10);

    auto* iconLabel = new QLabel(panel);
    iconLabel->setObjectName(QStringLiteral("reportSuccessIcon"));
    iconLabel->setFixedSize(40, 40);
    const QPixmap reportIcon(QStringLiteral(":/icons/report_icon.png"));
    if (!reportIcon.isNull()) {
        iconLabel->setPixmap(reportIcon.scaled(QSize(36, 36), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    iconLabel->setContentsMargins(0, 4, 0, 0);
    iconLabel->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(iconLabel);

    auto* titleLabel = new QLabel(QStringLiteral("신고 완료"), panel);
    titleLabel->setObjectName(QStringLiteral("reportSuccessTitle"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);
    panelLayout->addLayout(headerLayout);

    messageLabel_ = new QLabel(panel);
    messageLabel_->setObjectName(QStringLiteral("reportSuccessMessage"));
    messageLabel_->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(messageLabel_);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(1);

    auto* confirmButton = new QPushButton(QStringLiteral("확인"), panel);
    confirmButton->setObjectName(QStringLiteral("reportSuccessConfirmButton"));
    confirmButton->setCursor(Qt::PointingHandCursor);
    confirmButton->setDefault(true);
    connect(confirmButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(confirmButton);
    panelLayout->addLayout(buttonLayout);

    setChannelNumber(minimumChannelNumber);
}

/**
 * @brief               완료 문구에 표시할 채널 번호를 설정합니다.
 * @param channelNumber 1부터 4까지의 채널 번호
 */
void ReportSuccessDialog::setChannelNumber(int channelNumber) {
    const int boundedChannelNumber = qBound(minimumChannelNumber, channelNumber, maximumChannelNumber);
    messageLabel_->setText(QStringLiteral("%1 채널을 안전 센터에 신고하였습니다!").arg(boundedChannelNumber));
}
