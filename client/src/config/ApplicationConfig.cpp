#include "config/ApplicationConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <limits>
#include <utility>

namespace {
constexpr int requiredChannelCount = 4;

bool isValidTopicFilter(const QString& filter) {
    const QStringList levels = filter.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (levels.isEmpty()) {
        return false;
    }

    for (qsizetype index = 0; index < levels.size(); ++index) {
        const QString& level = levels[index];
        if (level.contains(QLatin1Char('#')) && (level != QStringLiteral("#") || index != levels.size() - 1)) {
            return false;
        }
        if (level.contains(QLatin1Char('+')) && level != QStringLiteral("+")) {
            return false;
        }
    }
    return true;
}

bool readObject(const QJsonObject& parent, const QString& key, QJsonObject& value, QString& error) {
    const QJsonValue candidate = parent.value(key);
    if (!candidate.isObject()) {
        error = QStringLiteral("Missing object: %1").arg(key);
        return false;
    }
    value = candidate.toObject();
    return true;
}

bool readString(const QJsonObject& object, const QString& key, QString& value, QString& error) {
    const QJsonValue candidate = object.value(key);
    if (!candidate.isString() || candidate.toString().trimmed().isEmpty()) {
        error = QStringLiteral("%1 must be a non-empty string").arg(key);
        return false;
    }
    value = candidate.toString().trimmed();
    return true;
}

bool readBoolean(const QJsonObject& object, const QString& key, bool& value, QString& error) {
    const QJsonValue candidate = object.value(key);
    if (!candidate.isBool()) {
        error = QStringLiteral("%1 must be a boolean").arg(key);
        return false;
    }
    value = candidate.toBool();
    return true;
}

bool readInteger(const QJsonObject& object, const QString& key, qint64 minimum, qint64 maximum, qint64& value,
                 QString& error) {
    const QJsonValue candidate = object.value(key);
    if (!candidate.isDouble()) {
        error = QStringLiteral("%1 must be an integer").arg(key);
        return false;
    }

    const qint64 integer = candidate.toInteger(std::numeric_limits<qint64>::min());
    if (integer == std::numeric_limits<qint64>::min() || static_cast<double>(integer) != candidate.toDouble() ||
        integer < minimum || integer > maximum) {
        error = QStringLiteral("%1 must be between %2 and %3").arg(key).arg(minimum).arg(maximum);
        return false;
    }
    value = integer;
    return true;
}

bool readDouble(const QJsonObject& object, const QString& key, double minimum, double maximum, double& value,
                QString& error) {
    const QJsonValue candidate = object.value(key);
    if (!candidate.isDouble() || candidate.toDouble() < minimum || candidate.toDouble() > maximum) {
        error = QStringLiteral("%1 must be between %2 and %3").arg(key).arg(minimum).arg(maximum);
        return false;
    }
    value = candidate.toDouble();
    return true;
}

bool readInt(const QJsonObject& object, const QString& key, int minimum, int maximum, int& value, QString& error) {
    qint64 parsed = 0;
    if (!readInteger(object, key, minimum, maximum, parsed, error)) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool readSubscription(const QJsonObject& topics, const QString& key, MqttSubscription& subscription, QString& error) {
    QJsonObject subscriptionObject;
    if (!readObject(topics, key, subscriptionObject, error) ||
        !readString(subscriptionObject, QStringLiteral("filter"), subscription.topicFilter, error)) {
        error = QStringLiteral("mqtt.topics.%1: %2").arg(key, error);
        return false;
    }

    if (!isValidTopicFilter(subscription.topicFilter)) {
        error = QStringLiteral("mqtt.topics.%1.filter is not a valid MQTT topic filter").arg(key);
        return false;
    }

    int qos = 0;
    if (!readInt(subscriptionObject, QStringLiteral("qos"), 0, 2, qos, error)) {
        error = QStringLiteral("mqtt.topics.%1: %2").arg(key, error);
        return false;
    }
    subscription.qos = static_cast<quint8>(qos);
    return true;
}

bool parseWindow(const QJsonObject& root, ApplicationWindowConfig& config, QString& error) {
    QJsonObject application;
    return readObject(root, QStringLiteral("application"), application, error) &&
           readInt(application, QStringLiteral("windowWidth"), 800, 7680, config.width, error) &&
           readInt(application, QStringLiteral("windowHeight"), 600, 4320, config.height, error);
}

bool parseStreams(const QJsonObject& video, QVector<StreamConfig>& streams, QString& error) {
    const QJsonValue streamValue = video.value(QStringLiteral("streams"));
    if (!streamValue.isArray() || streamValue.toArray().size() != requiredChannelCount) {
        error = QStringLiteral("video.streams must contain exactly %1 channels").arg(requiredChannelCount);
        return false;
    }

    QSet<QString> cameraIds;
    QSet<int> channelIndexes;
    const QJsonArray streamArray = streamValue.toArray();
    streams.reserve(streamArray.size());
    for (qsizetype index = 0; index < streamArray.size(); ++index) {
        if (!streamArray[index].isObject()) {
            error = QStringLiteral("video.streams[%1] must be an object").arg(index);
            return false;
        }

        const QJsonObject streamObject = streamArray[index].toObject();
        StreamConfig stream;
        if (!readString(streamObject, QStringLiteral("cameraId"), stream.cameraId, error) ||
            !readString(streamObject, QStringLiteral("name"), stream.name, error) ||
            !readString(streamObject, QStringLiteral("url"), stream.url, error) ||
            !readInt(streamObject, QStringLiteral("channelIndex"), 0, requiredChannelCount - 1, stream.channelIndex,
                     error) ||
            !readBoolean(streamObject, QStringLiteral("enabled"), stream.enabled, error)) {
            error = QStringLiteral("video.streams[%1]: %2").arg(index).arg(error);
            return false;
        }

        const QUrl streamUrl(stream.url);
        if (!streamUrl.isValid() ||
            (streamUrl.scheme() != QStringLiteral("rtsp") && streamUrl.scheme() != QStringLiteral("rtsps"))) {
            error = QStringLiteral("video.streams[%1].url must be a valid RTSP URL").arg(index);
            return false;
        }
        if (cameraIds.contains(stream.cameraId) || channelIndexes.contains(stream.channelIndex)) {
            error = QStringLiteral("video.streams contains a duplicate cameraId or channelIndex");
            return false;
        }

        cameraIds.insert(stream.cameraId);
        channelIndexes.insert(stream.channelIndex);
        streams.append(std::move(stream));
    }
    return true;
}

bool parseBlurConfig(const QJsonObject& receiver, BlurProcessorConfig& config, QString& error) {
    QJsonObject blur;
    qint64 maximumHistorySize = 0;
    return readObject(receiver, QStringLiteral("blur"), blur, error) &&
           readInteger(blur, QStringLiteral("syncOffsetMs"), 0, 10000, config.syncOffsetMsec, error) &&
           readInteger(blur, QStringLiteral("historyMs"), 100, 120000, config.historyMsec, error) &&
           readInteger(blur, QStringLiteral("matchToleranceMs"), 0, 10000, config.matchToleranceMsec, error) &&
           readInteger(blur, QStringLiteral("holdLastMetadataMs"), 0, 10000, config.holdLastMetadataMsec, error) &&
           readInteger(blur, QStringLiteral("maximumHistorySize"), 1, 10000, maximumHistorySize, error) &&
           (config.maximumHistorySize = static_cast<qsizetype>(maximumHistorySize), true) &&
           readInteger(blur, QStringLiteral("sourceRestartGapMs"), 100, 120000, config.sourceRestartGapMsec, error) &&
           readInteger(blur, QStringLiteral("sourceTimestampRestartThresholdMs"), 100, 120000,
                       config.sourceTimestampRestartThresholdMsec, error) &&
           readDouble(blur, QStringLiteral("paddingRatio"), 0.0, 1.0, config.paddingRatio, error) &&
           readInt(blur, QStringLiteral("maximumCornerRadius"), 0, 512, config.maximumCornerRadius, error) &&
           readInt(blur, QStringLiteral("radiusDivisor"), 1, 100, config.radiusDivisor, error) &&
           readInt(blur, QStringLiteral("minimumRadius"), 1, 512, config.minimumRadius, error) &&
           readInt(blur, QStringLiteral("maximumRadius"), config.minimumRadius, 2048, config.maximumRadius, error) &&
           readInt(blur, QStringLiteral("debugLogIntervalMs"), 0, 600000, config.debugLogIntervalMsec, error);
}

bool parseReceiverConfig(const QJsonObject& video, GstRtspReceiverConfig& config, QString& error) {
    QJsonObject receiver;
    qint64 udpBufferSize = 0;
    qint64 tcpTimeout = 0;
    qint64 udpTimeout = 0;
    if (!readObject(video, QStringLiteral("receiver"), receiver, error) ||
        !readString(receiver, QStringLiteral("decoderMode"), config.decoderMode, error)) {
        return false;
    }
    config.decoderMode = config.decoderMode.toLower();
    if (config.decoderMode != QStringLiteral("auto") && config.decoderMode != QStringLiteral("software") &&
        config.decoderMode != QStringLiteral("d3d11")) {
        error = QStringLiteral("video.receiver.decoderMode must be auto, software or d3d11");
        return false;
    }

    return readInt(receiver, QStringLiteral("busPollIntervalMs"), 10, 5000, config.busPollIntervalMsec, error) &&
           readInt(receiver, QStringLiteral("latencyMs"), 0, 60000, config.latencyMsec, error) &&
           readBoolean(receiver, QStringLiteral("dropOnLatency"), config.dropOnLatency, error) &&
           readInt(receiver, QStringLiteral("initialPacketTimeoutMs"), 100, 600000, config.initialPacketTimeoutMsec,
                   error) &&
           readInt(receiver, QStringLiteral("initialFrameTimeoutMs"), 100, 600000, config.initialFrameTimeoutMsec,
                   error) &&
           readInt(receiver, QStringLiteral("maximumReconnectDelayMs"), 100, 600000, config.maximumReconnectDelayMsec,
                   error) &&
           readInt(receiver, QStringLiteral("authenticationFailureReconnectDelayMs"), 100, 3600000,
                   config.authenticationFailureReconnectDelayMsec, error) &&
           readInt(receiver, QStringLiteral("stallTimeoutMs"), 100, 600000, config.stallTimeoutMsec, error) &&
           readInteger(receiver, QStringLiteral("udpBufferSizeBytes"), 0, 1073741824, udpBufferSize, error) &&
           (config.udpBufferSizeBytes = static_cast<quint64>(udpBufferSize), true) &&
           readInteger(receiver, QStringLiteral("tcpTimeoutUs"), 0, 3600000000LL, tcpTimeout, error) &&
           (config.tcpTimeoutUsec = static_cast<quint64>(tcpTimeout), true) &&
           readInteger(receiver, QStringLiteral("udpTimeoutUs"), 0, 3600000000LL, udpTimeout, error) &&
           (config.udpTimeoutUsec = static_cast<quint64>(udpTimeout), true) &&
           readInt(receiver, QStringLiteral("probationPackets"), 0, 1000, config.probationPackets, error) &&
           readBoolean(receiver, QStringLiteral("rtspKeepAlive"), config.rtspKeepAlive, error) &&
           readBoolean(receiver, QStringLiteral("udpReconnect"), config.udpReconnect, error) &&
           readBoolean(receiver, QStringLiteral("addReferenceTimestampMeta"), config.addReferenceTimestampMeta,
                       error) &&
           readInt(receiver, QStringLiteral("decodeQueueMaximumBuffers"), 1, 1000, config.decodeQueueMaximumBuffers,
                   error) &&
           readInteger(receiver, QStringLiteral("decodeQueueMaximumTimeMs"), 0, 60000,
                       config.decodeQueueMaximumTimeMsec, error) &&
           readInt(receiver, QStringLiteral("renderQueueMaximumBuffers"), 1, 1000, config.renderQueueMaximumBuffers,
                   error) &&
           readInteger(receiver, QStringLiteral("renderQueueMaximumTimeMs"), 0, 60000,
                       config.renderQueueMaximumTimeMsec, error) &&
           readBoolean(receiver, QStringLiteral("sinkQos"), config.sinkQos, error) &&
           readBoolean(receiver, QStringLiteral("sinkSync"), config.sinkSync, error) &&
           readBoolean(receiver, QStringLiteral("sinkAsync"), config.sinkAsync, error) &&
           readInteger(receiver, QStringLiteral("minimumLoadingMs"), 0, 60000, config.minimumLoadingMsec, error) &&
           readInt(receiver, QStringLiteral("reconnectSpreadMs"), 1, 600000, config.reconnectSpreadMsec, error) &&
           parseBlurConfig(receiver, config.blur, error);
}

bool parseVideo(const QJsonObject& root, VideoRuntimeConfig& config, QString& error) {
    QJsonObject video;
    return readObject(root, QStringLiteral("video"), video, error) &&
           readInt(video, QStringLiteral("initialStartDelayMs"), 0, 600000, config.initialStartDelayMsec, error) &&
           readInt(video, QStringLiteral("receiverStartSpacingMs"), 0, 600000, config.receiverStartSpacingMsec,
                   error) &&
           parseStreams(video, config.streams, error) && parseReceiverConfig(video, config.receiver, error);
}

bool parseMqtt(const QJsonObject& root, MqttRuntimeConfig& config, QString& clientIdPrefix, QString& error) {
    QJsonObject mqtt;
    if (!readObject(root, QStringLiteral("mqtt"), mqtt, error) ||
        !readString(mqtt, QStringLiteral("host"), config.connection.host, error)) {
        return false;
    }

    int port = 0;
    if (!readInt(mqtt, QStringLiteral("port"), 1, 65535, port, error) ||
        !readString(mqtt, QStringLiteral("caCertificatePath"), config.connection.caCertificatePath, error) ||
        !readString(mqtt, QStringLiteral("clientIdPrefix"), clientIdPrefix, error) ||
        !readInt(mqtt, QStringLiteral("keepAliveSeconds"), 1, 65535, config.connection.keepAliveSeconds, error) ||
        !readInt(mqtt, QStringLiteral("reconnectIntervalMs"), 100, 600000, config.connection.reconnectIntervalMsec,
                 error) ||
        !readBoolean(mqtt, QStringLiteral("debugLogging"), config.connection.debugLogging, error) ||
        !readInt(mqtt, QStringLiteral("blurDebugLogIntervalMs"), 0, 600000, config.blurDebugLogIntervalMsec, error)) {
        return false;
    }
    config.riskDebugLogIntervalMsec = config.blurDebugLogIntervalMsec;
    if (mqtt.contains(QStringLiteral("riskDebugLogIntervalMs")) &&
        !readInt(mqtt, QStringLiteral("riskDebugLogIntervalMs"), 0, 600000, config.riskDebugLogIntervalMsec, error)) {
        return false;
    }
    config.connection.port = static_cast<quint16>(port);

    qint64 maximumDebugPayloadLength = 0;
    if (!readInteger(mqtt, QStringLiteral("maximumDebugPayloadLength"), 0, 1048576, maximumDebugPayloadLength, error)) {
        return false;
    }
    config.maximumDebugPayloadLength = static_cast<qsizetype>(maximumDebugPayloadLength);

    QJsonObject topics;
    if (!readObject(mqtt, QStringLiteral("topics"), topics, error) ||
        !readSubscription(topics, QStringLiteral("controllerStatus"), config.topics.controllerStatus, error) ||
        !readSubscription(topics, QStringLiteral("centralStatus"), config.topics.centralStatus, error) ||
        !readSubscription(topics, QStringLiteral("sensorAlive"), config.topics.sensorAlive, error) ||
        !readSubscription(topics, QStringLiteral("centralEvent"), config.topics.centralEvent, error) ||
        !readSubscription(topics, QStringLiteral("risk"), config.topics.risk, error) ||
        !readSubscription(topics, QStringLiteral("blur"), config.topics.blur, error)) {
        return false;
    }

    QJsonObject dispatcher;
    return readObject(mqtt, QStringLiteral("dispatcher"), dispatcher, error) &&
           readInt(dispatcher, QStringLiteral("blurFlushIntervalMs"), 1, 60000, config.dispatcher.blurFlushIntervalMsec,
                   error) &&
           readInt(dispatcher, QStringLiteral("blurMaximumPendingFramesPerChannel"), 1, 1000,
                   config.dispatcher.blurMaximumPendingFramesPerChannel, error) &&
           readInt(dispatcher, QStringLiteral("blurSourceRestartGapMs"), 100, 600000,
                   config.dispatcher.blurSourceRestartGapMsec, error) &&
           readInteger(dispatcher, QStringLiteral("blurTimestampRestartThresholdMs"), 100, 600000,
                       config.dispatcher.blurTimestampRestartThresholdMsec, error) &&
           readInt(dispatcher, QStringLiteral("riskFlushIntervalMs"), 1, 60000, config.dispatcher.riskFlushIntervalMsec,
                   error) &&
           readInt(dispatcher, QStringLiteral("riskSourceRestartGapMs"), 100, 600000,
                   config.dispatcher.riskSourceRestartGapMsec, error) &&
           readInteger(dispatcher, QStringLiteral("riskTimestampRollbackResetMs"), 100, 600000,
                       config.dispatcher.riskTimestampRollbackResetMsec, error);
}

QString resolveConfigPath(QString& error) {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString explicitPath = environment.value(QStringLiteral("VEDA_CONFIG_FILE")).trimmed();
    if (!explicitPath.isEmpty()) {
        const QFileInfo fileInfo(explicitPath);
        if (!fileInfo.isFile()) {
            error = QStringLiteral("VEDA_CONFIG_FILE does not point to a file: %1").arg(explicitPath);
            return {};
        }
        return fileInfo.absoluteFilePath();
    }

    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config/app_config.json")),
        QDir::current().filePath(QStringLiteral("config/app_config.json"))};
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    error = QStringLiteral("app_config.json was not found beside the executable or in the working directory");
    return {};
}

QString environmentValue(const QProcessEnvironment& environment, const QString& name, const QString& fallback) {
    const QString value = environment.value(name).trimmed();
    return value.isEmpty() ? fallback : value;
}

bool environmentFlag(const QProcessEnvironment& environment, const QString& name, bool fallback) {
    const QString value = environment.value(name).trimmed().toLower();
    if (value == QStringLiteral("1") || value == QStringLiteral("true") || value == QStringLiteral("on") ||
        value == QStringLiteral("yes")) {
        return true;
    }
    if (value == QStringLiteral("0") || value == QStringLiteral("false") || value == QStringLiteral("off") ||
        value == QStringLiteral("no")) {
        return false;
    }
    return fallback;
}

void applyEnvironmentOverrides(ApplicationConfig& config, const QString& clientIdPrefix) {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    config.mqtt.connection.host =
        environmentValue(environment, QStringLiteral("VEDA_MQTT_HOST"), config.mqtt.connection.host);
    config.mqtt.connection.caCertificatePath =
        environmentValue(environment, QStringLiteral("VEDA_MQTT_CA_FILE"), config.mqtt.connection.caCertificatePath);
    config.mqtt.connection.debugLogging =
        environmentFlag(environment, QStringLiteral("VEDA_MQTT_DEBUG"), config.mqtt.connection.debugLogging);
    config.video.receiver.decoderMode =
        environmentValue(environment, QStringLiteral("QTCCTV_DECODER_MODE"), config.video.receiver.decoderMode)
            .toLower();

    bool portValid = false;
    const uint configuredPort = environment.value(QStringLiteral("VEDA_MQTT_PORT")).toUInt(&portValid);
    if (portValid && configuredPort > 0 && configuredPort <= 65535) {
        config.mqtt.connection.port = static_cast<quint16>(configuredPort);
    }

    const QString generatedClientId =
        QStringLiteral("%1-%2").arg(clientIdPrefix, QUuid::createUuid().toString(QUuid::WithoutBraces));
    config.mqtt.connection.clientId =
        environmentValue(environment, QStringLiteral("VEDA_MQTT_CLIENT_ID"), generatedClientId);

    bool blurOffsetValid = false;
    const int blurOffset = environment.value(QStringLiteral("QTCCTV_BLUR_SYNC_OFFSET_MS")).toInt(&blurOffsetValid);
    if (blurOffsetValid && blurOffset >= 0 && blurOffset <= 10000) {
        config.video.receiver.blur.syncOffsetMsec = blurOffset;
    }

    for (int index = 0; index < config.video.streams.size(); ++index) {
        const QString variableName = QStringLiteral("VEDA_RTSP_URL_%1").arg(index + 1);
        config.video.streams[index].url = environmentValue(environment, variableName, config.video.streams[index].url);
    }
}

bool validateEffectiveConfig(const ApplicationConfig& config, QString& error) {
    if (config.video.receiver.decoderMode != QStringLiteral("auto") &&
        config.video.receiver.decoderMode != QStringLiteral("software") &&
        config.video.receiver.decoderMode != QStringLiteral("d3d11")) {
        error = QStringLiteral("Effective decoder mode must be auto, software or d3d11");
        return false;
    }

    for (qsizetype index = 0; index < config.video.streams.size(); ++index) {
        const QUrl streamUrl(config.video.streams[index].url);
        if (!streamUrl.isValid() ||
            (streamUrl.scheme() != QStringLiteral("rtsp") && streamUrl.scheme() != QStringLiteral("rtsps"))) {
            error = QStringLiteral("Effective RTSP URL for channel %1 is invalid").arg(index + 1);
            return false;
        }
    }

    if (config.mqtt.connection.clientId.trimmed().isEmpty()) {
        error = QStringLiteral("Effective MQTT client ID is empty");
        return false;
    }
    return true;
}
}  // namespace

/**
 * @brief  외부 JSON 설정을 검증하고 환경 변수 override를 적용합니다.
 * @return 설정값, 원본 경로 및 오류를 포함한 로드 결과
 */
ApplicationConfigLoadResult ApplicationConfigLoader::load() {
    ApplicationConfigLoadResult result;
    result.sourcePath = resolveConfigPath(result.error);
    if (result.sourcePath.isEmpty()) {
        return result;
    }

    QFile file(result.sourcePath);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        result.error = QStringLiteral("Failed to open configuration: %1").arg(file.errorString());
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("Invalid configuration JSON at offset %1: %2")
                           .arg(parseError.offset)
                           .arg(parseError.errorString());
        return result;
    }

    QString clientIdPrefix;
    const QJsonObject root = document.object();
    if (!parseWindow(root, result.config.window, result.error) ||
        !parseVideo(root, result.config.video, result.error) ||
        !parseMqtt(root, result.config.mqtt, clientIdPrefix, result.error)) {
        return result;
    }

    applyEnvironmentOverrides(result.config, clientIdPrefix);
    if (!validateEffectiveConfig(result.config, result.error)) {
        return result;
    }
    result.successful = true;
    return result;
}
