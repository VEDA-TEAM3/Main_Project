#pragma once

#include <QDialog>

class QLabel;

class ReportSuccessDialog final : public QDialog {
public:
    explicit ReportSuccessDialog(QWidget* parent = nullptr);

    void setChannelNumber(int channelNumber);

private:
    QLabel* messageLabel_ = nullptr;
};
