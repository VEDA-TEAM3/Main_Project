#include "network/QtMqttTransport.h"

#include <QDebug>
#include <QFileInfo>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QtMqtt/QMqttClient>
#include <QtMqtt/QMqttSubscription>
#include <QtMqtt/QMqttTopicName>
#include <utility>

/**
 * @brief         Qt MQTT 기반 TLS 전송 객체를 생성합니다.
 * @param config  broker, 인증서 및 재생성 가능한 client 설정
 */
QtMqttTransport::QtMqttTransport(MqttConnectionConfig config) : config_(std::move(config)) {
    reconnectTimer_.setInterval(config_.reconnectIntervalMsec);
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &QtMqttTransport::connectToBroker);
}

QtMqttTransport::~QtMqttTransport() { stop(); }

/**
 * @brief      상위 gateway가 사용할 전송 이벤트 callback을 등록합니다.
 * @param callbacks 연결, 메시지 및 오류 callback 묶음
 */
void QtMqttTransport::setCallbacks(MqttTransportCallbacks callbacks) { callbacks_ = std::move(callbacks); }

/**
 * @brief Qt MQTT client를 현재 thread에서 만들고 TLS 접속을 시작합니다.
 */
void QtMqttTransport::start() {
    if (client_) {
        return;
    }

    stopping_ = false;
    client_ = std::make_unique<QMqttClient>();
    client_->setHostname(config_.host);
    client_->setPort(config_.port);
    client_->setClientId(config_.clientId);
    client_->setKeepAlive(static_cast<quint16>(config_.keepAliveSeconds));
    client_->setCleanSession(true);

    connect(client_.get(), &QMqttClient::connected, this, [this]() {
        if (config_.debugLogging) {
            qInfo().noquote() << QStringLiteral("[MQTT] Connected host=%1 port=%2").arg(config_.host).arg(config_.port);
        }
        if (callbacks_.connectionChanged) {
            callbacks_.connectionChanged(true);
        }
    });
    connect(client_.get(), &QMqttClient::disconnected, this, [this]() {
        if (config_.debugLogging) {
            qInfo().noquote() << QStringLiteral("[MQTT] Disconnected");
        }
        if (callbacks_.connectionChanged) {
            callbacks_.connectionChanged(false);
        }
        scheduleReconnect();
    });
    connect(client_.get(), &QMqttClient::messageReceived, this,
            [this](const QByteArray& payload, const QMqttTopicName& topic) {
                if (callbacks_.messageReceived) {
                    callbacks_.messageReceived(payload, topic.name());
                }
            });
    connect(client_.get(), &QMqttClient::errorChanged, this, [this](QMqttClient::ClientError error) {
        if (error != QMqttClient::NoError) {
            reportError(QStringLiteral("MQTT client error: %1").arg(static_cast<int>(error)));
            scheduleReconnect();
        }
    });

    connectToBroker();
}

/**
 * @brief 재접속을 중지하고 Qt MQTT client를 생성 thread에서 정리합니다.
 */
void QtMqttTransport::stop() {
    stopping_ = true;
    reconnectTimer_.stop();

    if (!client_) {
        return;
    }

    disconnect(client_.get(), nullptr, this, nullptr);
    if (client_->state() != QMqttClient::Disconnected) {
        client_->disconnectFromHost();
    }
    client_.reset();

    if (callbacks_.connectionChanged) {
        callbacks_.connectionChanged(false);
    }
}

/**
 * @brief               현재 연결에 토픽 filter를 등록합니다.
 * @param subscription  토픽 filter와 QoS
 * @return              구독 요청 객체가 생성되면 true
 */
bool QtMqttTransport::subscribe(const MqttSubscription& subscription) {
    if (!client_ || client_->state() != QMqttClient::Connected) {
        return false;
    }

    QMqttSubscription* mqttSubscription = client_->subscribe(subscription.topicFilter, subscription.qos);
    if (!mqttSubscription) {
        reportError(QStringLiteral("MQTT subscribe failed: %1").arg(subscription.topicFilter));
        return false;
    }

    const QString topic = subscription.topicFilter;
    connect(
        mqttSubscription, &QMqttSubscription::stateChanged, this,
        [this, mqttSubscription, topic](QMqttSubscription::SubscriptionState state) {
            if (state == QMqttSubscription::Subscribed) {
                if (config_.debugLogging) {
                    qInfo().noquote() << QStringLiteral("[MQTT SUBSCRIBED] topic=%1 qos=%2")
                                             .arg(topic)
                                             .arg(static_cast<int>(mqttSubscription->qos()));
                }
                return;
            }

            if (state == QMqttSubscription::Error) {
                reportError(QStringLiteral("MQTT subscription error: %1 (%2)").arg(topic, mqttSubscription->reason()));
            }
        });

    if (config_.debugLogging) {
        qInfo().noquote()
            << QStringLiteral("[MQTT SUB PENDING] topic=%1 qos=%2").arg(topic).arg(static_cast<int>(subscription.qos));
    }
    return true;
}

/** @brief 유효한 CA 인증서를 적용해 TLS broker 접속을 요청합니다. */
void QtMqttTransport::connectToBroker() {
    if (stopping_ || !client_ || client_->state() != QMqttClient::Disconnected) {
        return;
    }

    const QList<QSslCertificate> certificates = QSslCertificate::fromPath(config_.caCertificatePath);
    if (certificates.isEmpty()) {
        reportError(QStringLiteral("MQTT CA certificate not found or invalid: %1")
                        .arg(QFileInfo(config_.caCertificatePath).absoluteFilePath()));
        scheduleReconnect();
        return;
    }

    QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
    sslConfiguration.setCaCertificates(certificates);
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    sslConfiguration.setProtocol(QSsl::TlsV1_2OrLater);

    if (config_.debugLogging) {
        qInfo().noquote() << QStringLiteral("[MQTT] Connecting host=%1 port=%2 clientId=%3")
                                 .arg(config_.host)
                                 .arg(config_.port)
                                 .arg(config_.clientId);
    }
    client_->connectToHostEncrypted(sslConfiguration);
}

/** @brief 중복 timer 없이 다음 broker 접속 시도를 예약합니다. */
void QtMqttTransport::scheduleReconnect() {
    if (!stopping_ && client_ && client_->state() == QMqttClient::Disconnected && !reconnectTimer_.isActive()) {
        reconnectTimer_.start();
    }
}

/** @brief 전송 계층 오류를 gateway callback으로 전달합니다. */
void QtMqttTransport::reportError(QString detail) {
    if (callbacks_.errorOccurred) {
        callbacks_.errorOccurred(std::move(detail));
    }
}
