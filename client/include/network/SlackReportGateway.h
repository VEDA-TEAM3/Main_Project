#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QString>

#include "network/ReportGateway.h"

class QNetworkReply;
class QJsonObject;

class SlackReportGateway final : public ReportGateway {
    Q_OBJECT

public:
    explicit SlackReportGateway(QString botToken, QString targetType, QString recipientUserId,
                                QString recipientChannelId, QObject* parent = nullptr);

    void sendReport(ReportRequest request) override;

private:
    void processNextReport();
    void openDirectMessage(const ReportRequest& request);
    void postMessage(const ReportRequest& request);
    void finishWithFailure(const ReportRequest& request, const QString& error);
    void finishRequest();
    QNetworkReply* postJson(const QString& method, const QByteArray& payload);
    QString parseSlackError(QNetworkReply* reply, const QByteArray& response, QJsonObject& responseObject) const;
    QByteArray createOpenConversationPayload() const;
    QByteArray createMessagePayload(const ReportRequest& request) const;
    bool usesChannelTarget() const;
    QString configurationError() const;

    QNetworkAccessManager networkManager_;
    QQueue<ReportRequest> pendingReports_;
    QString botToken_;
    QString targetType_;
    QString recipientUserId_;
    QString recipientChannelId_;
    QString destinationChannelId_;
    bool requestInProgress_ = false;
};
