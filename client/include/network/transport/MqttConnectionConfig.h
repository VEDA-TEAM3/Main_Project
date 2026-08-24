#pragma once

#include <QString>

struct MqttConnectionConfig {
    QString host;
    quint16 port = 0;
    QString caCertificatePath;
    QString clientId;
    /// CONNECT 패킷에 실을 broker 계정. 비우면 익명 접속이고, 그때 broker에 발행 ACL이 없으면
    /// 누구든 risk/blur/장비 상태 토픽에 이 클라이언트의 입력을 직접 넣을 수 있다.
    /// 파일보다 VEDA_MQTT_USERNAME/VEDA_MQTT_PASSWORD 환경 변수를 쓰는 쪽이 낫다
    QString userName;
    QString password;
    int keepAliveSeconds = 0;
    int reconnectIntervalMsec = 0;
    bool debugLogging = false;
};
