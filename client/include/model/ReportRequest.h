#pragma once

#include <QDateTime>
#include <QString>

struct ReportRequest {
    QString reportId;
    QString riskLevel;
    QString detail;
    QDateTime reportedAt;
    int channelNumber = 1;
};
