#pragma once

#include <QDialog>

class QLabel;

class InformationDialog final : public QDialog {
public:
    explicit InformationDialog(QWidget* parent = nullptr);

    void setContent(const QString& title, const QString& message);

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
};
