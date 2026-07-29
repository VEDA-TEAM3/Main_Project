#include "network/SlackReportGateway.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <utility>

namespace {
constexpr int requestTimeoutMsec = 10000;
const QString slackApiBaseUrl = QStringLiteral("https://slack.com/api/");
}  // namespace

/**
 * @brief                  Slack DM 신고 전송기를 생성합니다.
 * @param botToken         Slack Bot User OAuth Token
 * @param recipientUserId  신고 메시지를 받을 Slack 사용자 ID
 * @param parent           Qt 객체 소유권을 연결할 부모 객체
 */
SlackReportGateway::SlackReportGateway(QString botToken, QString targetType, QString recipientUserId,
                                       QString recipientChannelId, QObject* parent)
    : ReportGateway(parent),
      botToken_(std::move(botToken)),
      targetType_(std::move(targetType)),
      recipientUserId_(std::move(recipientUserId)),
      recipientChannelId_(std::move(recipientChannelId)) {
    targetType_ = targetType_.trimmed().toLower();
    if (targetType_.isEmpty()) {
        targetType_ = recipientChannelId_.trimmed().isEmpty() ? QStringLiteral("dm") : QStringLiteral("channel");
    }
    if (usesChannelTarget()) {
        destinationChannelId_ = recipientChannelId_.trimmed();
    }
}

/**
 * @brief         신고 요청을 순차 전송 대기열에 추가합니다.
 * @param request 채널, 위험 단계, 신고 시각을 포함한 신고 정보
 */
void SlackReportGateway::sendReport(ReportRequest request) {
    const QString error = configurationError();
    if (!error.isEmpty()) {
        const int channelNumber = request.channelNumber;
        QTimer::singleShot(0, this, [this, channelNumber, error]() { emit reportFailed(channelNumber, error); });
        return;
    }

    pendingReports_.enqueue(std::move(request));
    processNextReport();
}

/**
 * @brief 대기 중인 신고를 하나씩 처리합니다.
 */
void SlackReportGateway::processNextReport() {
    if (requestInProgress_ || pendingReports_.isEmpty()) {
        return;
    }

    requestInProgress_ = true;
    const ReportRequest request = pendingReports_.head();

    if (!usesChannelTarget() && destinationChannelId_.isEmpty()) {
        openDirectMessage(request);
        return;
    }

    postMessage(request);
}

/**
 * @brief         신고 대상 사용자와의 DM 채널을 열거나 기존 채널을 조회합니다.
 * @param request 현재 처리 중인 신고 정보
 */
void SlackReportGateway::openDirectMessage(const ReportRequest& request) {
    QNetworkReply* reply = postJson(QStringLiteral("conversations.open"), createOpenConversationPayload());
    connect(reply, &QNetworkReply::finished, this, [this, reply, request]() {
        const QByteArray response = reply->readAll();
        QJsonObject responseObject;
        const QString error = parseSlackError(reply, response, responseObject);
        reply->deleteLater();

        if (!error.isEmpty()) {
            finishWithFailure(request, error);
            return;
        }

        destinationChannelId_ =
            responseObject.value(QStringLiteral("channel")).toObject().value(QStringLiteral("id")).toString();
        if (destinationChannelId_.isEmpty()) {
            finishWithFailure(request, QStringLiteral("Slack DM 채널 ID가 응답에 없습니다."));
            return;
        }

        postMessage(request);
    });
}

/**
 * @brief         열린 Slack DM 채널로 신고 메시지를 전송합니다.
 * @param request 전송할 신고 정보
 */
void SlackReportGateway::postMessage(const ReportRequest& request) {
    QNetworkReply* reply = postJson(QStringLiteral("chat.postMessage"), createMessagePayload(request));
    connect(reply, &QNetworkReply::finished, this, [this, reply, request]() {
        const QByteArray response = reply->readAll();
        QJsonObject responseObject;
        const QString error = parseSlackError(reply, response, responseObject);
        reply->deleteLater();

        if (!error.isEmpty()) {
            if (!usesChannelTarget() && error == QStringLiteral("channel_not_found")) {
                destinationChannelId_.clear();
            }
            finishWithFailure(request, error);
            return;
        }

        emit reportSucceeded(request.channelNumber, request.reportId);
        finishRequest();
    });
}

/**
 * @brief         실패 결과를 전달하고 다음 신고를 처리합니다.
 * @param request 실패한 신고 정보
 * @param error   사용자에게 표시할 오류 내용
 */
void SlackReportGateway::finishWithFailure(const ReportRequest& request, const QString& error) {
    emit reportFailed(request.channelNumber, error);
    finishRequest();
}

/**
 * @brief 현재 신고를 대기열에서 제거하고 다음 신고를 시작합니다.
 */
void SlackReportGateway::finishRequest() {
    if (!pendingReports_.isEmpty()) {
        pendingReports_.dequeue();
    }
    requestInProgress_ = false;
    QTimer::singleShot(0, this, &SlackReportGateway::processNextReport);
}

/**
 * @brief         Slack Web API로 JSON POST 요청을 전송합니다.
 * @param method  호출할 Slack API 메서드
 * @param payload JSON 요청 본문
 * @return        비동기 응답 객체
 */
QNetworkReply* SlackReportGateway::postJson(const QString& method, const QByteArray& payload) {
    QNetworkRequest request(QUrl(slackApiBaseUrl + method));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + botToken_.toUtf8());
    request.setTransferTimeout(requestTimeoutMsec);
    return networkManager_.post(request, payload);
}

/**
 * @brief                HTTP 및 Slack JSON 응답을 공통 검증합니다.
 * @param reply          네트워크 응답 객체
 * @param response       Slack 응답 본문
 * @param responseObject 파싱된 JSON 객체를 받을 출력 인자
 * @return               성공하면 빈 문자열, 실패하면 오류 설명
 */
QString SlackReportGateway::parseSlackError(QNetworkReply* reply, const QByteArray& response,
                                            QJsonObject& responseObject) const {
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return QStringLiteral("Slack 응답 JSON을 해석할 수 없습니다.");
    }

    responseObject = document.object();
    if (!responseObject.value(QStringLiteral("ok")).toBool(false)) {
        return responseObject.value(QStringLiteral("error")).toString(QStringLiteral("Slack API 오류"));
    }

    return {};
}

/**
 * @brief 1:1 DM 채널 생성 요청 본문을 만듭니다.
 * @return UTF-8 JSON 본문
 */
QByteArray SlackReportGateway::createOpenConversationPayload() const {
    QJsonObject payload;
    payload.insert(QStringLiteral("users"), recipientUserId_);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

/**
 * @brief         Slack 신고 메시지 본문을 만듭니다.
 * @param request 채널과 위험 정보를 포함한 신고 정보
 * @return        UTF-8 JSON 본문
 */
QByteArray SlackReportGateway::createMessagePayload(const ReportRequest& request) const {
    const QString channelName = QStringLiteral("CH %1").arg(request.channelNumber, 2, 10, QLatin1Char('0'));
    const QString reportedAt = request.reportedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString text = QStringLiteral(
                             "[Wise AI 안전 신고]\n\n"
                             "채널: %1\n"
                             "신고 시각: %2\n"
                             "위험 수준: %3\n"
                             "신고 ID: %4\n"
                             "내용: %5")
                             .arg(channelName, reportedAt, request.riskLevel, request.reportId, request.detail);

    QJsonObject payload;
    payload.insert(QStringLiteral("channel"), destinationChannelId_);
    payload.insert(QStringLiteral("text"), text);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

/**
 * @brief  설정된 Slack 신고 목적지가 채널인지 확인합니다.
 * @return 채널 전송이면 true, 사용자 DM 전송이면 false
 */
bool SlackReportGateway::usesChannelTarget() const { return targetType_ == QStringLiteral("channel"); }

/**
 * @brief  Slack 신고 환경 변수의 필수값과 목적지 종류를 검증합니다.
 * @return 설정이 유효하면 빈 문자열, 잘못됐으면 오류 설명
 */
QString SlackReportGateway::configurationError() const {
    if (botToken_.trimmed().isEmpty()) {
        return QStringLiteral("Slack 신고 환경 변수 SLACK_BOT_TOKEN이 없습니다.");
    }
    if (targetType_ != QStringLiteral("dm") && targetType_ != QStringLiteral("channel")) {
        return QStringLiteral("SLACK_REPORT_TARGET은 dm 또는 channel이어야 합니다.");
    }
    if (usesChannelTarget() && recipientChannelId_.trimmed().isEmpty()) {
        return QStringLiteral("채널 신고 환경 변수 SLACK_REPORT_CHANNEL_ID가 없습니다.");
    }
    if (!usesChannelTarget() && recipientUserId_.trimmed().isEmpty()) {
        return QStringLiteral("DM 신고 환경 변수 SLACK_REPORT_USER_ID가 없습니다.");
    }
    return {};
}
