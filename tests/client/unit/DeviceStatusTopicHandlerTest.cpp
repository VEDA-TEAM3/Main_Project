#include <QByteArray>
#include <QString>
#include <cstdio>

#include "network/routing/DeviceStatusTopicHandler.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failureCount;
    }
}

DeviceStatusTopicHandler handler() {
    MqttTopicsConfig config;
    config.controllerStatus.topicFilter = QStringLiteral("veda/hw/ch/+/status");
    config.centralStatus.topicFilter = QStringLiteral("veda/hw/status");
    config.sensorAlive.topicFilter = QStringLiteral("veda/ch/+/alive");
    config.centralEvent.topicFilter = QStringLiteral("veda/events");
    return DeviceStatusTopicHandler(config, 4);
}

QByteArray statusPayload(bool hardwareAlive) {
    return QStringLiteral(R"({
        "v":1,"ch":2,"ts":1787000000123,
        "cameraAlive":true,"hardwareAlive":%1,
        "sirenOn":true,"buzzerOn":true,
        "ledRed":true,"ledYellow":false,"ledGreen":false
    })")
        .arg(hardwareAlive ? QStringLiteral("true") : QStringLiteral("false"))
        .toUtf8();
}

void checkDeadHardwareGatesStaleOutputs() {
    const auto topicHandler = handler();
    MqttMessageBatch messages;
    QString error;

    check(topicHandler.handle(statusPayload(false), QStringLiteral("veda/hw/ch/2/status"), messages, error),
          "dead hardware snapshot must remain a valid status message");
    check(messages.reports.size() == 1, "one status report must be emitted");
    const DeviceStatusReport dead = messages.reports.value(0);
    check(!dead.hardwareAlive, "hardware must be marked offline");
    check(!dead.hasOutputState, "stale siren/LED fields must not be exposed as active output state");

    messages = {};
    error.clear();
    check(topicHandler.handle(statusPayload(true), QStringLiteral("veda/hw/ch/2/status"), messages, error),
          "alive hardware snapshot must parse");
    check(messages.reports.value(0).hasOutputState, "live siren/LED fields must be exposed");
}

void checkMalformedStatusFailsClosed() {
    const auto topicHandler = handler();
    MqttMessageBatch messages;
    QString error;
    check(!topicHandler.handle(statusPayload(true), QStringLiteral("veda/hw/ch/1/status"), messages, error),
          "topic/payload channel mismatch must be rejected");
    check(!topicHandler.handle(R"({"v":2})", QStringLiteral("veda/hw/ch/2/status"), messages, error),
          "unsupported schema must be rejected");
}
}  // namespace

int main() {
    checkDeadHardwareGatesStaleOutputs();
    checkMalformedStatusFailsClosed();

    if (failureCount != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }
    std::printf("DeviceStatusTopicHandler checks passed\n");
    return 0;
}
