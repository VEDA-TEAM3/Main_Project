#pragma once

#include <QObject>
#include <QString>

#include "model/ReportRequest.h"

class ReportGateway : public QObject {
    Q_OBJECT

public:
    explicit ReportGateway(QObject* parent = nullptr);
    ~ReportGateway() override;

    virtual void sendReport(ReportRequest request) = 0;

signals:
    void reportSucceeded(int channelNumber, QString reportId);
    void reportFailed(int channelNumber, QString error);
};
