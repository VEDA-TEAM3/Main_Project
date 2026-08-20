#include "config/ApplicationConfig.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <utility>

namespace {
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

/**
 * @brief         minX/minY/maxX/maxY 네 값을 월드 상자로 읽습니다.
 * @param object  상자 값을 담은 JSON 객체
 * @param bounds  변환된 월드 상자 (성공했을 때만 씁니다)
 * @param error   검증 실패 원인
 */
bool readWorldBounds(const QJsonObject& object, QRectF& bounds, QString& error) {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    if (!readDouble(object, QStringLiteral("minX"), -1000000000.0, 1000000000.0, minX, error) ||
        !readDouble(object, QStringLiteral("minY"), -1000000000.0, 1000000000.0, minY, error) ||
        !readDouble(object, QStringLiteral("maxX"), -1000000000.0, 1000000000.0, maxX, error) ||
        !readDouble(object, QStringLiteral("maxY"), -1000000000.0, 1000000000.0, maxY, error) || maxX <= minX ||
        maxY <= minY) {
        if (error.isEmpty()) {
            error = QStringLiteral("max must be greater than min");
        }
        return false;
    }

    bounds = QRectF(minX, minY, maxX - minX, maxY - minY);
    return true;
}

/**
 * @brief         물리 CCTV 구역별 월드 상자를 읽습니다.
 * @param world   digitalTwin.world JSON 객체
 * @param config  구역 상자를 채울 월드 설정
 * @param error   검증 실패 원인
 *
 * @details 선택 항목이다. 없으면 zones가 비어 있는 채로 남고, resolveWorldZoneCount가
 *          video.areas 개수만큼 늘려 DigitalTwinWorldConfig::zoneBounds가 bounds를 균등하게
 *          갈라 쓰게 한다. 구역들이 도면에서 멀리 떨어져 있을 때만 실제 상자가 필요하다.
 */
bool readWorldZones(const QJsonObject& world, DigitalTwinWorldConfig& config, QString& error) {
    if (!world.contains(QStringLiteral("zones"))) {
        return true;
    }

    const QJsonValue zonesValue = world.value(QStringLiteral("zones"));
    if (!zonesValue.isArray() || zonesValue.toArray().isEmpty() ||
        zonesValue.toArray().size() > digitalTwinMaximumZoneCount) {
        error = QStringLiteral("digitalTwin.world.zones must contain 1 to %1 boxes").arg(digitalTwinMaximumZoneCount);
        return false;
    }

    const QJsonArray zoneArray = zonesValue.toArray();
    config.zones.resize(zoneArray.size());
    for (qsizetype index = 0; index < zoneArray.size(); ++index) {
        if (!zoneArray.at(index).isObject() ||
            !readWorldBounds(zoneArray.at(index).toObject(), config.zones[index], error)) {
            error = QStringLiteral("digitalTwin.world.zones[%1] is invalid: %2").arg(index).arg(error);
            return false;
        }
    }
    return true;
}

/**
 * @brief         구역 상자 개수를 실제 CCTV 구역 수에 맞춥니다.
 * @param config  video와 digitalTwin이 모두 채워진 설정
 * @param error   검증 실패 원인
 *
 * @details 맵은 video.areas 하나당 구역 하나를 그리므로 두 개수가 어긋나면 상자를 못 찾은
 *          구역의 객체가 엉뚱한 배율로 그려진다. 상자를 아예 안 적었으면 균등 가르기를 쓰도록
 *          빈 상자로 길이만 맞춰 둔다.
 */
bool resolveWorldZoneCount(ApplicationConfig& config, QString& error) {
    const qsizetype areaCount = config.video.areas.size();
    if (config.digitalTwin.world.zones.isEmpty()) {
        config.digitalTwin.world.zones.resize(areaCount);
        return true;
    }

    if (config.digitalTwin.world.zones.size() != areaCount) {
        error = QStringLiteral("digitalTwin.world.zones has %1 boxes but video.areas has %2 areas")
                    .arg(config.digitalTwin.world.zones.size())
                    .arg(areaCount);
        return false;
    }
    return true;
}

bool parseDigitalTwin(const QJsonObject& root, DigitalTwinRuntimeConfig& config, QString& error) {
    if (!root.contains(QStringLiteral("digitalTwin"))) {
        return true;
    }

    QJsonObject digitalTwin;
    QJsonObject world;
    qint64 maximumHistorySize = 0;
    if (!readObject(root, QStringLiteral("digitalTwin"), digitalTwin, error) ||
        !readInt(digitalTwin, QStringLiteral("renderIntervalMs"), 10, 1000, config.renderIntervalMsec, error) ||
        !readInt(digitalTwin, QStringLiteral("snapshotPublishIntervalMs"), 10, 5000, config.snapshotPublishIntervalMsec,
                 error) ||
        !readInt(digitalTwin, QStringLiteral("frameExpiryMs"), 100, 120000, config.frameExpiryMsec, error) ||
        !readInteger(digitalTwin, QStringLiteral("fadeInMs"), 0, 5000, config.fadeInMsec, error) ||
        !readInteger(digitalTwin, QStringLiteral("missingGraceMs"), 0, 5000, config.missingGraceMsec, error) ||
        !readInteger(digitalTwin, QStringLiteral("fadeOutMs"), 0, 5000, config.fadeOutMsec, error) ||
        !readInteger(digitalTwin, QStringLiteral("maximumHistorySize"), 2, 128, maximumHistorySize, error) ||
        !readInt(digitalTwin, QStringLiteral("diagnosticsIntervalMs"), 0, 600000, config.diagnosticsIntervalMsec,
                 error) ||
        !readObject(digitalTwin, QStringLiteral("world"), world, error) ||
        !readBoolean(world, QStringLiteral("fixedBoundsEnabled"), config.world.fixedBoundsEnabled, error) ||
        !readBoolean(world, QStringLiteral("invertY"), config.world.invertY, error)) {
        error = QStringLiteral("digitalTwin: %1").arg(error);
        return false;
    }

    if (digitalTwin.contains(QStringLiteral("positionTransitionMs"))) {
        if (!readInteger(digitalTwin, QStringLiteral("positionTransitionMs"), 0, 5000, config.positionTransitionMsec,
                         error)) {
            error = QStringLiteral("digitalTwin: %1").arg(error);
            return false;
        }
    } else if (digitalTwin.contains(QStringLiteral("renderDelayMs"))) {
        // Backward compatibility: the old render delay becomes only the local position transition duration.
        if (!readInteger(digitalTwin, QStringLiteral("renderDelayMs"), 0, 5000, config.positionTransitionMsec, error)) {
            error = QStringLiteral("digitalTwin: %1").arg(error);
            return false;
        }
    }

    if (digitalTwin.contains(QStringLiteral("icons"))) {
        QJsonObject icons;
        if (!readObject(digitalTwin, QStringLiteral("icons"), icons, error) ||
            (icons.contains(QStringLiteral("vehiclePx")) &&
             !readInt(icons, QStringLiteral("vehiclePx"), 8, 512, config.icons.vehiclePixels, error)) ||
            (icons.contains(QStringLiteral("pedestrianPx")) &&
             !readInt(icons, QStringLiteral("pedestrianPx"), 8, 512, config.icons.pedestrianPixels, error))) {
            error = QStringLiteral("digitalTwin.icons: %1").arg(error);
            return false;
        }
    }

    qint64 automaticBoundsMinimumSamples = config.world.automaticBoundsMinimumSamples;
    qint64 automaticBoundsMaximumSamples = config.world.automaticBoundsMaximumSamples;
    if ((world.contains(QStringLiteral("automaticBoundsWarmupMs")) &&
         !readInt(world, QStringLiteral("automaticBoundsWarmupMs"), 0, 10000, config.world.automaticBoundsWarmupMsec,
                  error)) ||
        (world.contains(QStringLiteral("automaticBoundsMinimumSamples")) &&
         !readInteger(world, QStringLiteral("automaticBoundsMinimumSamples"), 2, 4096, automaticBoundsMinimumSamples,
                      error)) ||
        (world.contains(QStringLiteral("automaticBoundsMaximumSamples")) &&
         !readInteger(world, QStringLiteral("automaticBoundsMaximumSamples"), 2, 16384, automaticBoundsMaximumSamples,
                      error)) ||
        (world.contains(QStringLiteral("automaticBoundsPaddingRatio")) &&
         !readDouble(world, QStringLiteral("automaticBoundsPaddingRatio"), 0.0, 1.0,
                     config.world.automaticBoundsPaddingRatio, error)) ||
        (world.contains(QStringLiteral("automaticBoundsOutlierFraction")) &&
         !readDouble(world, QStringLiteral("automaticBoundsOutlierFraction"), 0.0, 0.25,
                     config.world.automaticBoundsOutlierFraction, error)) ||
        automaticBoundsMaximumSamples < automaticBoundsMinimumSamples) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "automaticBoundsMaximumSamples must be greater than or equal to "
                "automaticBoundsMinimumSamples");
        }
        error = QStringLiteral("digitalTwin.world automatic bounds are invalid: %1").arg(error);
        return false;
    }
    config.world.automaticBoundsMinimumSamples = static_cast<qsizetype>(automaticBoundsMinimumSamples);
    config.world.automaticBoundsMaximumSamples = static_cast<qsizetype>(automaticBoundsMaximumSamples);

    if (!readWorldBounds(world, config.world.bounds, error)) {
        error = QStringLiteral("digitalTwin.world bounds are invalid: %1").arg(error);
        return false;
    }
    if (!readWorldZones(world, config.world, error)) {
        return false;
    }

    config.maximumHistorySize = static_cast<qsizetype>(maximumHistorySize);
    return true;
}

bool parseStreams(const QJsonObject& video, int channelCount, QVector<StreamConfig>& streams, QString& error) {
    const QJsonValue streamValue = video.value(QStringLiteral("streams"));
    if (!streamValue.isArray() || streamValue.toArray().size() != channelCount) {
        error = QStringLiteral("video.streams must contain exactly %1 channels").arg(channelCount);
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
            !readInt(streamObject, QStringLiteral("channelIndex"), 0, channelCount - 1, stream.channelIndex, error) ||
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

    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        if (!channelIndexes.contains(channelIndex)) {
            error = QStringLiteral("video.streams channelIndex values must be contiguous from 0 to %1")
                        .arg(channelCount - 1);
            return false;
        }
    }

    std::sort(streams.begin(), streams.end(), [](const StreamConfig& left, const StreamConfig& right) {
        return left.channelIndex < right.channelIndex;
    });
    return true;
}

bool parseVideoAreas(const QJsonObject& video, const QVector<StreamConfig>& streams, VideoRuntimeConfig& config,
                     QString& error) {
    const QJsonValue areaValue = video.value(QStringLiteral("areas"));
    if (!areaValue.isArray() || areaValue.toArray().isEmpty()) {
        error = QStringLiteral("video.areas must contain at least one area");
        return false;
    }

    QHash<QString, int> channelIndexByCameraId;
    for (const StreamConfig& stream : streams) {
        channelIndexByCameraId.insert(stream.cameraId, stream.channelIndex);
    }

    QSet<QString> areaIds;
    QSet<int> assignedChannelIndexes;
    const QJsonArray areaArray = areaValue.toArray();
    config.areas.reserve(areaArray.size());

    for (qsizetype areaIndex = 0; areaIndex < areaArray.size(); ++areaIndex) {
        if (!areaArray[areaIndex].isObject()) {
            error = QStringLiteral("video.areas[%1] must be an object").arg(areaIndex);
            return false;
        }

        const QJsonObject areaObject = areaArray[areaIndex].toObject();
        VideoAreaConfig area;
        if (!readString(areaObject, QStringLiteral("areaId"), area.areaId, error) ||
            !readString(areaObject, QStringLiteral("name"), area.name, error)) {
            error = QStringLiteral("video.areas[%1]: %2").arg(areaIndex).arg(error);
            return false;
        }
        if (areaIds.contains(area.areaId)) {
            error = QStringLiteral("video.areas contains duplicate areaId: %1").arg(area.areaId);
            return false;
        }

        const QJsonValue streamIdsValue = areaObject.value(QStringLiteral("streamIds"));
        if (!streamIdsValue.isArray() || streamIdsValue.toArray().size() != videoChannelsPerArea) {
            error = QStringLiteral("video.areas[%1].streamIds must contain exactly %2 camera IDs")
                        .arg(areaIndex)
                        .arg(videoChannelsPerArea);
            return false;
        }

        const QJsonArray streamIds = streamIdsValue.toArray();
        area.channelIndexes.reserve(streamIds.size());
        for (qsizetype slotIndex = 0; slotIndex < streamIds.size(); ++slotIndex) {
            if (!streamIds[slotIndex].isString() || streamIds[slotIndex].toString().trimmed().isEmpty()) {
                error = QStringLiteral("video.areas[%1].streamIds[%2] must be a non-empty string")
                            .arg(areaIndex)
                            .arg(slotIndex);
                return false;
            }

            const QString cameraId = streamIds[slotIndex].toString().trimmed();
            if (!channelIndexByCameraId.contains(cameraId)) {
                error = QStringLiteral("video.areas[%1] references unknown cameraId: %2").arg(areaIndex).arg(cameraId);
                return false;
            }

            const int channelIndex = channelIndexByCameraId.value(cameraId);
            const int expectedChannelIndex =
                videoGlobalChannelIndex(static_cast<int>(areaIndex), static_cast<int>(slotIndex));
            if (channelIndex != expectedChannelIndex) {
                error = QStringLiteral("video.areas[%1].streamIds[%2] must map to channelIndex %3")
                            .arg(areaIndex)
                            .arg(slotIndex)
                            .arg(expectedChannelIndex);
                return false;
            }
            if (assignedChannelIndexes.contains(channelIndex)) {
                error = QStringLiteral("video.areas assigns cameraId more than once: %1").arg(cameraId);
                return false;
            }
            assignedChannelIndexes.insert(channelIndex);
            area.channelIndexes.append(channelIndex);
        }

        areaIds.insert(area.areaId);
        config.areas.append(std::move(area));
    }

    QString initialAreaId;
    if (!readString(video, QStringLiteral("initialAreaId"), initialAreaId, error)) {
        error = QStringLiteral("video: %1").arg(error);
        return false;
    }

    config.initialAreaIndex = -1;
    for (qsizetype areaIndex = 0; areaIndex < config.areas.size(); ++areaIndex) {
        if (config.areas[areaIndex].areaId == initialAreaId) {
            config.initialAreaIndex = static_cast<int>(areaIndex);
            break;
        }
    }
    if (config.initialAreaIndex < 0) {
        error = QStringLiteral("video.initialAreaId does not match a configured area: %1").arg(initialAreaId);
        return false;
    }

    if (assignedChannelIndexes.size() != streams.size()) {
        error = QStringLiteral("Every video stream must belong to exactly one video area");
        return false;
    }
    return true;
}

bool parseBlurConfig(const QJsonObject& receiver, BlurProcessorConfig& config, QString& error) {
    QJsonObject blur;
    qint64 maximumHistorySize = 0;
    if (!readObject(receiver, QStringLiteral("blur"), blur, error)) {
        return false;
    }

    return readInteger(blur, QStringLiteral("syncOffsetMs"), 0, 10000, config.syncOffsetMsec, error) &&
           readInteger(blur, QStringLiteral("historyMs"), 100, 120000, config.historyMsec, error) &&
           readInteger(blur, QStringLiteral("matchToleranceMs"), 0, 10000, config.matchToleranceMsec, error) &&
           readInteger(blur, QStringLiteral("holdLastMetadataMs"), 0, 10000, config.holdLastMetadataMsec, error) &&
           readInteger(blur, QStringLiteral("maxExtrapolationMs"), 0, 2000, config.maximumExtrapolationMsec, error) &&
           readInteger(blur, QStringLiteral("maximumHistorySize"), 1, 10000, maximumHistorySize, error) &&
           (config.maximumHistorySize = static_cast<qsizetype>(maximumHistorySize), true) &&
           readInteger(blur, QStringLiteral("sourceRestartGapMs"), 100, 120000, config.sourceRestartGapMsec, error) &&
           readDouble(blur, QStringLiteral("paddingRatio"), 0.0, 1.0, config.paddingRatio, error) &&
           readInt(blur, QStringLiteral("radiusDivisor"), 1, 100, config.radiusDivisor, error) &&
           readInt(blur, QStringLiteral("minimumRadius"), 1, 512, config.minimumRadius, error) &&
           readInt(blur, QStringLiteral("maximumRadius"), config.minimumRadius, 2048, config.maximumRadius, error) &&
           readInt(blur, QStringLiteral("debugLogIntervalMs"), 0, 600000, config.debugLogIntervalMsec, error);
}

bool parseVideoPreprocessingConfig(const QJsonObject& receiver, VideoPreprocessingSettings& settings, QString& error) {
    if (!receiver.contains(QStringLiteral("preprocessing"))) {
        return true;
    }

    QJsonObject preprocessing;
    QString preset;
    if (!readObject(receiver, QStringLiteral("preprocessing"), preprocessing, error) ||
        !readBoolean(preprocessing, QStringLiteral("enabled"), settings.enabled, error) ||
        !readString(preprocessing, QStringLiteral("preset"), preset, error) ||
        !readInt(preprocessing, QStringLiteral("brightness"), -20, 20, settings.brightness, error) ||
        !readDouble(preprocessing, QStringLiteral("contrast"), 0.8, 1.2, settings.contrast, error) ||
        !readDouble(preprocessing, QStringLiteral("gamma"), 0.8, 1.4, settings.gamma, error)) {
        return false;
    }

    preset = preset.trimmed().toLower();
    if (preset == QStringLiteral("custom")) {
        settings.preset = VideoPreprocessingPreset::Custom;
    } else if (preset == QStringLiteral("day")) {
        settings.preset = VideoPreprocessingPreset::Day;
    } else if (preset == QStringLiteral("night")) {
        settings.preset = VideoPreprocessingPreset::Night;
    } else {
        error = QStringLiteral("video.receiver.preprocessing.preset must be custom, day or night");
        return false;
    }
    return true;
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

    // 두 값은 함께 있어야 의미가 있다. 한쪽만 주면 화면비가 조용히 틀어진다
    const bool hasProcessingWidth = receiver.contains(QStringLiteral("processingWidth"));
    const bool hasProcessingHeight = receiver.contains(QStringLiteral("processingHeight"));
    if (hasProcessingWidth != hasProcessingHeight) {
        error = QStringLiteral("video.receiver.processingWidth and processingHeight must be set together");
        return false;
    }
    if (hasProcessingWidth &&
        (!readInt(receiver, QStringLiteral("processingWidth"), 0, 7680, config.processingWidth, error) ||
         !readInt(receiver, QStringLiteral("processingHeight"), 0, 4320, config.processingHeight, error))) {
        return false;
    }
    if ((config.processingWidth > 0) != (config.processingHeight > 0)) {
        error = QStringLiteral("video.receiver.processingWidth and processingHeight must both be 0 or both positive");
        return false;
    }

    if (receiver.contains(QStringLiteral("alignmentDelayMs")) &&
        !readInteger(receiver, QStringLiteral("alignmentDelayMs"), 0, 5000, config.alignmentDelayMsec, error)) {
        return false;
    }
    const bool parsed =
        readInt(receiver, QStringLiteral("busPollIntervalMs"), 10, 5000, config.busPollIntervalMsec, error) &&
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
        readBoolean(receiver, QStringLiteral("addReferenceTimestampMeta"), config.addReferenceTimestampMeta, error) &&
        readInteger(receiver, QStringLiteral("decodeQueueMaximumTimeMs"), 0, 60000, config.decodeQueueMaximumTimeMsec,
                    error) &&
        readInteger(receiver, QStringLiteral("renderQueueMaximumTimeMs"), 0, 60000, config.renderQueueMaximumTimeMsec,
                    error) &&
        readBoolean(receiver, QStringLiteral("sinkQos"), config.sinkQos, error) &&
        readBoolean(receiver, QStringLiteral("sinkSync"), config.sinkSync, error) &&
        readBoolean(receiver, QStringLiteral("sinkAsync"), config.sinkAsync, error) &&
        readInteger(receiver, QStringLiteral("minimumLoadingMs"), 0, 60000, config.minimumLoadingMsec, error) &&
        readInt(receiver, QStringLiteral("reconnectSpreadMs"), 1, 600000, config.reconnectSpreadMsec, error) &&
        parseVideoPreprocessingConfig(receiver, config.preprocessing, error) &&
        parseBlurConfig(receiver, config.blur, error);
    if (!parsed) {
        return false;
    }

    // 정렬 지연은 sink의 ts-offset으로만 만든다. GstBaseSink 문서상 ts-offset은 clock 동기화 경로에서
    // 쓰이므로 sync=false면 값이 조용히 무시되고, 블러가 영상보다 alignmentDelayMs만큼 앞서 나간다
    if (config.alignmentDelayMsec > 0 && !config.sinkSync) {
        error = QStringLiteral(
            "video.receiver.alignmentDelayMs requires sinkSync=true (the delay is applied as the sink ts-offset)");
        return false;
    }

    return true;
}

bool parseVideo(const QJsonObject& root, VideoRuntimeConfig& config, QString& error) {
    QJsonObject video;
    if (!readObject(root, QStringLiteral("video"), video, error)) {
        return false;
    }

    const QJsonValue areaValue = video.value(QStringLiteral("areas"));
    if (!areaValue.isArray() || areaValue.toArray().isEmpty() ||
        areaValue.toArray().size() > digitalTwinMaximumZoneCount) {
        error = QStringLiteral("video.areas must contain 1 to %1 areas").arg(digitalTwinMaximumZoneCount);
        return false;
    }

    const int channelCount = static_cast<int>(areaValue.toArray().size()) * videoChannelsPerArea;
    return readInt(video, QStringLiteral("initialStartDelayMs"), 0, 600000, config.initialStartDelayMsec, error) &&
           readInt(video, QStringLiteral("receiverStartSpacingMs"), 0, 600000, config.receiverStartSpacingMsec,
                   error) &&
           parseStreams(video, channelCount, config.streams, error) &&
           parseVideoAreas(video, config.streams, config, error) && parseReceiverConfig(video, config.receiver, error);
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
           readInt(dispatcher, QStringLiteral("blurSourceRestartGapMs"), 100, 600000,
                   config.dispatcher.blurSourceRestartGapMsec, error) &&
           readInteger(dispatcher, QStringLiteral("blurTimestampRestartThresholdMs"), 100, 600000,
                       config.dispatcher.blurTimestampRestartThresholdMsec, error) &&
           readInt(dispatcher, QStringLiteral("riskFlushIntervalMs"), 1, 60000, config.dispatcher.riskFlushIntervalMsec,
                   error) &&
           readInt(dispatcher, QStringLiteral("riskSourceRestartGapMs"), 100, 600000,
                   config.dispatcher.riskSourceRestartGapMsec, error);
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

/**
 * @brief         로그 카테고리별 on/off를 읽어 각 구성 요소 설정에 반영합니다.
 * @param root    설정 파일 루트 객체
 * @param config  플래그를 반영할 애플리케이션 설정
 * @param error   검증 실패 원인
 * @return        파싱과 반영에 성공하면 true
 *
 * @details logging 블록과 그 안의 키는 모두 선택 사항이다. 없으면 각 플래그의 기본값을 쓴다.
 *          오류 로그는 어떤 플래그로도 끄지 않는다.
 */
bool parseLogging(const QJsonObject& root, ApplicationConfig& config, QString& error) {
    QJsonObject logging;
    if (root.contains(QStringLiteral("logging")) && !readObject(root, QStringLiteral("logging"), logging, error)) {
        return false;
    }

    const auto readFlag = [&logging, &error](const QString& key, bool& value) {
        return !logging.contains(key) || readBoolean(logging, key, value, error);
    };

    bool blurApply = config.video.receiver.blur.debugLogIntervalMsec > 0;
    if (!readFlag(QStringLiteral("enabled"), config.logging.enabled) ||
        !readFlag(QStringLiteral("mqttConnection"), config.mqtt.connection.debugLogging) ||
        !readFlag(QStringLiteral("mqttStatusPayload"), config.mqtt.logStatusPayload) ||
        !readFlag(QStringLiteral("mqttRisk"), config.mqtt.logRisk) ||
        !readFlag(QStringLiteral("mqttBlur"), config.mqtt.logBlur) ||
        !readFlag(QStringLiteral("blurApply"), blurApply) ||
        !readFlag(QStringLiteral("blurDispatch"), config.mqtt.dispatcher.logBlurDispatch) ||
        !readFlag(QStringLiteral("riskDispatch"), config.mqtt.dispatcher.logRiskDispatch) ||
        !readFlag(QStringLiteral("topview"), config.digitalTwin.debugLogging) ||
        !readFlag(QStringLiteral("topviewDetail"), config.digitalTwin.debugDetail)) {
        error = QStringLiteral("logging is invalid: %1").arg(error);
        return false;
    }

    if (logging.contains(QStringLiteral("topviewDetailIntervalMs"))) {
        qint64 detailIntervalMsec = config.digitalTwin.debugDetailIntervalMsec;
        if (!readInteger(logging, QStringLiteral("topviewDetailIntervalMs"), 0, 60000, detailIntervalMsec, error)) {
            error = QStringLiteral("logging is invalid: %1").arg(error);
            return false;
        }
        config.digitalTwin.debugDetailIntervalMsec = static_cast<int>(detailIntervalMsec);
    }

    // BlurProcessor는 주기값으로 로그를 켠다. 꺼졌으면 주기를 0으로 만들어 출력 경로를 막는다
    if (!blurApply) {
        config.video.receiver.blur.debugLogIntervalMsec = 0;
    }
    return true;
}

void applyLoggingMasterSwitch(ApplicationConfig& config) {
    if (config.logging.enabled) {
        return;
    }

    config.mqtt.connection.debugLogging = false;
    config.mqtt.logStatusPayload = false;
    config.mqtt.logRisk = false;
    config.mqtt.logBlur = false;
    config.mqtt.dispatcher.logBlurDispatch = false;
    config.mqtt.dispatcher.logRiskDispatch = false;
    config.digitalTwin.debugLogging = false;
    config.digitalTwin.debugDetail = false;
    config.video.receiver.blur.debugLogIntervalMsec = 0;
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
        !parseDigitalTwin(root, result.config.digitalTwin, result.error) ||
        !parseVideo(root, result.config.video, result.error) ||
        !parseMqtt(root, result.config.mqtt, clientIdPrefix, result.error) ||
        !parseLogging(root, result.config, result.error) || !resolveWorldZoneCount(result.config, result.error)) {
        return result;
    }

    result.config.mqtt.channelCount = static_cast<int>(result.config.video.streams.size());

    applyEnvironmentOverrides(result.config, clientIdPrefix);
    applyLoggingMasterSwitch(result.config);
    if (!validateEffectiveConfig(result.config, result.error)) {
        return result;
    }
    result.successful = true;
    return result;
}

namespace {
/**
 * @brief        새 구역의 월드 상자를 만듭니다.
 * @param zones  기존 digitalTwin.world.zones 배열
 *
 * @details RTSP 주소는 그 구역이 도면 어디에 있는지 알려 주지 않는다. 마지막 두 구역의 간격을
 *          그대로 이어 붙여 겹치지 않는 자리에 두고, 정확한 좌표는 사용자가 나중에 보정한다.
 *          크기를 그대로 물려받으므로 정사각형 상자라는 전제도 유지된다.
 */
QJsonObject nextZoneBox(const QJsonArray& zones) {
    const QJsonObject last = zones.last().toObject();
    const double minX = last.value(QStringLiteral("minX")).toDouble();
    const double minY = last.value(QStringLiteral("minY")).toDouble();
    const double maxX = last.value(QStringLiteral("maxX")).toDouble();
    const double maxY = last.value(QStringLiteral("maxY")).toDouble();

    double offsetX = (maxX - minX) * 1.25;
    double offsetY = 0.0;
    if (zones.size() >= 2) {
        const QJsonObject previous = zones.at(zones.size() - 2).toObject();
        offsetX = minX - previous.value(QStringLiteral("minX")).toDouble();
        offsetY = minY - previous.value(QStringLiteral("minY")).toDouble();
    }

    // 앞의 두 구역이 같은 자리에 적혀 있으면 간격이 0이라 새 구역이 그 위에 겹친다
    if (qFuzzyIsNull(offsetX) && qFuzzyIsNull(offsetY)) {
        offsetX = (maxX - minX) * 1.25;
    }

    return QJsonObject{{QStringLiteral("minX"), minX + offsetX},
                       {QStringLiteral("minY"), minY + offsetY},
                       {QStringLiteral("maxX"), maxX + offsetX},
                       {QStringLiteral("maxY"), maxY + offsetY}};
}

/**
 * @brief           RTSP 주소에 구역 계정을 채워 넣습니다.
 * @param address   사용자가 입력한 RTSP 주소
 * @param userName  구역 계정, 비어 있으면 주소를 그대로 둡니다
 * @param password  구역 비밀번호
 * @return          계정이 포함된 RTSP URL
 *
 * @details 주소에 이미 계정이 있으면 그 값을 우선한다(카메라마다 계정이 다른 구성).
 *          QUrl을 거치므로 '@'나 ':'가 든 비밀번호가 percent-encoding되어, 손으로 이어 붙일 때처럼
 *          호스트가 잘못 잘리지 않는다. GstRtspReceiver가 FullyDecoded로 다시 풀어 쓴다.
 */
QString addressWithCredentials(const QString& address, const QString& userName, const QString& password) {
    const QString trimmedAddress = address.trimmed();
    QUrl url(trimmedAddress);
    if (userName.isEmpty() || !url.isValid() || !url.userInfo().isEmpty()) {
        return trimmedAddress;
    }

    url.setUserName(userName);
    if (!password.isEmpty()) {
        url.setPassword(password);
    }
    return url.toString(QUrl::FullyEncoded);
}

/**
 * @brief                  구역 하나가 쓸 RTSP URL 네 개를 만들고 검증합니다.
 * @param streamAddresses  사용자가 입력한 주소 목록
 * @param streamUrls       계정까지 합친 결과
 * @return                 실패 원인, 성공하면 빈 문자열
 */
QString buildStreamUrls(const QStringList& streamAddresses, const QString& userName, const QString& password,
                        QStringList& streamUrls) {
    if (streamAddresses.size() != videoChannelsPerArea) {
        return QStringLiteral("RTSP 주소 %1개를 모두 입력하세요.").arg(videoChannelsPerArea);
    }

    streamUrls.clear();
    for (qsizetype index = 0; index < streamAddresses.size(); ++index) {
        const QString streamUrl = addressWithCredentials(streamAddresses.at(index), userName, password);
        const QUrl url(streamUrl);
        if (!url.isValid() || url.host().isEmpty() ||
            (url.scheme() != QStringLiteral("rtsp") && url.scheme() != QStringLiteral("rtsps"))) {
            return QStringLiteral("%1번 주소가 rtsp:// 형식이 아닙니다.").arg(index + 1);
        }
        streamUrls.append(streamUrl);
    }
    return {};
}

/** @brief 설정 파일을 읽어 JSON 객체로 돌려줍니다. */
QString readConfigDocument(const QString& configPath, QJsonObject& root) {
    QFile file(configPath);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        return QStringLiteral("설정 파일을 열지 못했습니다: %1").arg(file.errorString());
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return QStringLiteral("설정 파일을 읽지 못했습니다: %1").arg(parseError.errorString());
    }

    root = document.object();
    return {};
}

/**
 * @brief 설정 파일을 통째로 교체하고 원래 접근 권한을 되돌립니다.
 *
 * @details 쓰다 만 파일이 남으면 다음 실행이 아예 못 뜬다. QSaveFile은 임시 파일을 만들어
 *          원본을 교체하므로, 이 파일에 걸어 둔 접근 제한이 새 파일의 기본 권한으로 풀릴 수
 *          있다. RTSP 비밀번호가 들어 있는 파일이라 권한을 다시 씌운다.
 */
QString writeConfigDocument(const QString& configPath, const QJsonObject& root) {
    const QFile::Permissions originalPermissions = QFile::permissions(configPath);
    QSaveFile output(configPath);
    if (!output.open(QFile::WriteOnly | QFile::Text) ||
        output.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !output.commit()) {
        return QStringLiteral("설정 파일을 저장하지 못했습니다: %1").arg(output.errorString());
    }

    if (originalPermissions != QFile::Permissions() && !QFile::setPermissions(configPath, originalPermissions)) {
        qWarning() << "[ApplicationConfigWriter] Failed to restore configuration file permissions" << configPath;
    }
    return {};
}

/** @brief video.areas와 video.streams를 꺼내고 서로 개수가 맞는지 확인합니다. */
QString readVideoArrays(const QJsonObject& root, QJsonObject& video, QJsonArray& areas, QJsonArray& streams) {
    video = root.value(QStringLiteral("video")).toObject();
    areas = video.value(QStringLiteral("areas")).toArray();
    streams = video.value(QStringLiteral("streams")).toArray();
    if (areas.isEmpty() || streams.size() != areas.size() * videoChannelsPerArea) {
        return QStringLiteral("설정 파일의 구역과 채널 구성이 손상되어 있습니다.");
    }
    return {};
}
}  // namespace

/**
 * @brief 주소와 계정을 하나의 RTSP URL로 합칩니다.
 */
QString ApplicationConfigWriter::composeUrl(const QString& address, const QString& userName, const QString& password) {
    return addressWithCredentials(address, userName, password);
}

/**
 * @brief             구역 하나와 그 RTSP 채널들을 설정 파일 끝에 추가합니다.
 * @param configPath  ApplicationConfigLoadResult::sourcePath
 * @param areaName    화면에 표시할 구역 이름
 * @param streamUrls  구역당 채널 수만큼의 RTSP URL
 * @return            실패 원인, 성공하면 빈 문자열
 */
QString ApplicationConfigWriter::appendArea(const QString& configPath, const QString& areaName,
                                            const QStringList& streamAddresses, const QString& userName,
                                            const QString& password) {
    const QString trimmedName = areaName.trimmed();
    if (trimmedName.isEmpty()) {
        return QStringLiteral("구역 이름을 입력하세요.");
    }

    QStringList streamUrls;
    QString error = buildStreamUrls(streamAddresses, userName, password, streamUrls);
    if (!error.isEmpty()) {
        return error;
    }

    QJsonObject root;
    error = readConfigDocument(configPath, root);
    if (!error.isEmpty()) {
        return error;
    }

    QJsonObject video;
    QJsonArray areas;
    QJsonArray streams;
    error = readVideoArrays(root, video, areas, streams);
    if (!error.isEmpty()) {
        return error;
    }
    if (areas.size() >= digitalTwinMaximumZoneCount) {
        return QStringLiteral("구역은 최대 %1개까지 추가할 수 있습니다.").arg(digitalTwinMaximumZoneCount);
    }

    QSet<QString> usedCameraIds;
    QSet<QString> usedAreaIds;
    for (const QJsonValue& stream : streams) {
        usedCameraIds.insert(stream.toObject().value(QStringLiteral("cameraId")).toString());
    }
    for (const QJsonValue& area : areas) {
        usedAreaIds.insert(area.toObject().value(QStringLiteral("areaId")).toString());
    }

    // 사용자가 손으로 id를 바꿔 두었어도 충돌하지 않도록 빈 자리를 찾아 붙인다
    const auto uniqueIdentifier = [](const QString& format, int startNumber, const QSet<QString>& used) {
        int number = startNumber;
        QString candidate = format.arg(number, 2, 10, QLatin1Char('0'));
        while (used.contains(candidate)) {
            candidate = format.arg(++number, 2, 10, QLatin1Char('0'));
        }
        return candidate;
    };

    const int firstChannelIndex = static_cast<int>(streams.size());
    const QString areaId = uniqueIdentifier(QStringLiteral("area-%1"), static_cast<int>(areas.size()) + 1, usedAreaIds);

    QJsonArray streamIds;
    for (int localChannelIndex = 0; localChannelIndex < videoChannelsPerArea; ++localChannelIndex) {
        const int channelIndex = firstChannelIndex + localChannelIndex;
        const QString cameraId = uniqueIdentifier(QStringLiteral("cam-%1"), channelIndex + 1, usedCameraIds);
        usedCameraIds.insert(cameraId);
        streamIds.append(cameraId);
        streams.append(QJsonObject{
            {QStringLiteral("cameraId"), cameraId},
            {QStringLiteral("name"), QStringLiteral("CH - %1").arg(channelIndex + 1, 2, 10, QLatin1Char('0'))},
            {QStringLiteral("url"), streamUrls.at(localChannelIndex)},
            {QStringLiteral("channelIndex"), channelIndex},
            {QStringLiteral("enabled"), true}});
    }

    areas.append(QJsonObject{{QStringLiteral("areaId"), areaId},
                             {QStringLiteral("name"), trimmedName},
                             {QStringLiteral("streamIds"), streamIds}});
    video.insert(QStringLiteral("areas"), areas);
    video.insert(QStringLiteral("streams"), streams);
    root.insert(QStringLiteral("video"), video);

    // zones가 없으면 로더가 bounds를 균등하게 갈라 쓰므로 그대로 둔다
    QJsonObject digitalTwin = root.value(QStringLiteral("digitalTwin")).toObject();
    QJsonObject world = digitalTwin.value(QStringLiteral("world")).toObject();
    QJsonArray zones = world.value(QStringLiteral("zones")).toArray();
    if (!zones.isEmpty()) {
        zones.append(nextZoneBox(zones));
        world.insert(QStringLiteral("zones"), zones);
        digitalTwin.insert(QStringLiteral("world"), world);
        root.insert(QStringLiteral("digitalTwin"), digitalTwin);
    }

    return writeConfigDocument(configPath, root);
}

/**
 * @brief 이미 있는 구역의 이름과 RTSP 채널을 바꿔 씁니다.
 *
 * @details 구역 구성(개수, 채널 번호, 구역 상자)은 건드리지 않고 이름과 url만 갈아 끼운다.
 *          채널 순서는 areas[].streamIds가 정하므로 cameraId로 찾아 쓴다.
 */
QString ApplicationConfigWriter::updateArea(const QString& configPath, int areaIndex, const QString& areaName,
                                            const QStringList& streamAddresses, const QString& userName,
                                            const QString& password) {
    const QString trimmedName = areaName.trimmed();
    if (trimmedName.isEmpty()) {
        return QStringLiteral("구역 이름을 입력하세요.");
    }

    QStringList streamUrls;
    QString error = buildStreamUrls(streamAddresses, userName, password, streamUrls);
    if (!error.isEmpty()) {
        return error;
    }

    QJsonObject root;
    error = readConfigDocument(configPath, root);
    if (!error.isEmpty()) {
        return error;
    }

    QJsonObject video;
    QJsonArray areas;
    QJsonArray streams;
    error = readVideoArrays(root, video, areas, streams);
    if (!error.isEmpty()) {
        return error;
    }
    if (areaIndex < 0 || areaIndex >= areas.size()) {
        return QStringLiteral("설정 파일에 없는 구역입니다.");
    }

    QJsonObject area = areas.at(areaIndex).toObject();
    const QJsonArray streamIds = area.value(QStringLiteral("streamIds")).toArray();
    if (streamIds.size() != videoChannelsPerArea) {
        return QStringLiteral("구역의 채널 구성이 손상되어 있습니다.");
    }

    for (qsizetype localChannelIndex = 0; localChannelIndex < streamIds.size(); ++localChannelIndex) {
        const QString cameraId = streamIds.at(localChannelIndex).toString();
        qsizetype streamIndex = -1;
        for (qsizetype index = 0; index < streams.size(); ++index) {
            if (streams.at(index).toObject().value(QStringLiteral("cameraId")).toString() == cameraId) {
                streamIndex = index;
                break;
            }
        }
        if (streamIndex < 0) {
            return QStringLiteral("구역이 참조하는 카메라 %1을(를) 찾지 못했습니다.").arg(cameraId);
        }

        QJsonObject stream = streams.at(streamIndex).toObject();
        stream.insert(QStringLiteral("url"), streamUrls.at(localChannelIndex));
        streams.replace(streamIndex, stream);
    }

    area.insert(QStringLiteral("name"), trimmedName);
    areas.replace(areaIndex, area);
    video.insert(QStringLiteral("areas"), areas);
    video.insert(QStringLiteral("streams"), streams);
    root.insert(QStringLiteral("video"), video);

    return writeConfigDocument(configPath, root);
}
