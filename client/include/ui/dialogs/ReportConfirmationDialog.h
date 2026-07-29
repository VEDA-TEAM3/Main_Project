#pragma once

#include <QDialog>

class QLabel;

class ReportConfirmationDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ReportConfirmationDialog(QWidget* parent = nullptr);

    void setChannelNumber(int channelNumber);

signals:
    void reportConfirmed(int channelNumber);

private:
    QLabel* messageLabel_ = nullptr;
    int channelNumber_ = 1;
};
