#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdio>

#include "config/ApplicationConfig.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

QStringList zoneUrls(int firstChannelNumber) {
    QStringList urls;
    for (int offset = 0; offset < videoChannelsPerArea; ++offset) {
        urls.append(QStringLiteral("rtsp://user:pass@10.0.0.9:554/%1/media.smp").arg(firstChannelNumber + offset));
    }
    return urls;
}

/**
 * @brief 배포 기본 설정을 임시 파일로 복사해 원본을 건드리지 않고 검사합니다.
 *
 * @details 예제는 구역이 하나도 없는 최초 배포 상태다. 이후 검사는 여기에 구역을 붙여 나간다.
 */
QString prepareWorkingCopy() {
    const QString target = QDir(QDir::tempPath()).filePath(QStringLiteral("qtcctv_config_area_writer_check.json"));
    QFile::remove(target);
    if (!QFile::copy(QStringLiteral(QTCCTV_EXAMPLE_CONFIG_PATH), target)) {
        return {};
    }
    return target;
}

/**
 * @brief 구역이 하나도 없는 설정이 그대로 뜨고, 첫 구역을 붙여도 다시 뜨는지 검사합니다.
 *
 * @details 갓 배포한 프로그램이 여기서 걸리면 아무도 첫 구역을 넣을 수 없다. 첫 구역을 붙인
 *          직후에는 initialAreaId가 아직 없으므로 그 경로도 같이 본다.
 */
void checkEmptyConfigLoads(const QString& configPath) {
    const ApplicationConfigLoadResult empty = ApplicationConfigLoader::load();
    check(empty.successful, "a freshly deployed config with no zones must load");
    if (!empty.successful) {
        std::fprintf(stderr, "  load error: %s\n", qPrintable(empty.error));
        return;
    }
    check(empty.config.video.areas.isEmpty() && empty.config.video.streams.isEmpty(),
          "no zones must mean no areas and no channels");
    check(empty.config.mqtt.channelCount == 0, "the MQTT layer must start with no channels");

    check(ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 1구역"), zoneUrls(1)).isEmpty(),
          "the very first zone must be addable to an empty config");

    const ApplicationConfigLoadResult first = ApplicationConfigLoader::load();
    check(first.successful, "the config must still load once the first zone exists");
    if (!first.successful) {
        std::fprintf(stderr, "  load error: %s\n", qPrintable(first.error));
        return;
    }
    check(first.config.video.areas.size() == 1 && first.config.video.streams.size() == videoChannelsPerArea,
          "the first zone must bring its own channels");
    check(first.config.video.initialAreaIndex == 0,
          "the first zone must become the initial area even though initialAreaId is absent");
}

/**
 * @brief 두 번째 구역과 보정된 구역 상자를 심어 이후 검사의 출발점을 만듭니다.
 *
 * @details 상자를 적어 두어야 nextZoneBox가 간격을 이어 붙이는 경로를 검사할 수 있다.
 *          상자가 없으면 로더가 bounds를 균등하게 갈라 쓰므로 그 경로는 지나가지 않는다.
 */
bool seedSecondZone(const QString& configPath) {
    if (!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 2구역"), zoneUrls(5)).isEmpty()) {
        return false;
    }

    QFile file(configPath);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        return false;
    }
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    QJsonObject digitalTwin = root.value(QStringLiteral("digitalTwin")).toObject();
    QJsonObject world = digitalTwin.value(QStringLiteral("world")).toObject();
    world.insert(QStringLiteral("zones"), QJsonArray{QJsonObject{{QStringLiteral("minX"), 0.0},
                                                                 {QStringLiteral("minY"), 0.0},
                                                                 {QStringLiteral("maxX"), 10.0},
                                                                 {QStringLiteral("maxY"), 10.0}},
                                                     QJsonObject{{QStringLiteral("minX"), 12.785},
                                                                 {QStringLiteral("minY"), 0.0},
                                                                 {QStringLiteral("maxX"), 22.785},
                                                                 {QStringLiteral("maxY"), 10.0}}});
    digitalTwin.insert(QStringLiteral("world"), world);
    root.insert(QStringLiteral("digitalTwin"), digitalTwin);

    if (!file.open(QFile::WriteOnly | QFile::Truncate | QFile::Text)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

/** @brief 잘못된 입력은 파일을 건드리기 전에 거절하는지 검사합니다. */
void checkRejectedInput(const QString& configPath) {
    check(!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("  "), zoneUrls(9)).isEmpty(),
          "an empty zone name must be rejected");
    check(!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 3구역"), {}).isEmpty(),
          "a missing url list must be rejected");
    check(!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 3구역"), zoneUrls(9).mid(1)).isEmpty(),
          "fewer urls than channels per area must be rejected");
    check(!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 3구역"),
                                               {QStringLiteral("http://10.0.0.9/stream"), QStringLiteral("rtsp://a/1"),
                                                QStringLiteral("rtsp://a/2"), QStringLiteral("rtsp://a/3")})
               .isEmpty(),
          "a non-rtsp url must be rejected");

    const ApplicationConfigLoadResult unchanged = ApplicationConfigLoader::load();
    check(unchanged.successful && unchanged.config.video.areas.size() == 2,
          "a rejected request must leave the file untouched");
}

/**
 * @brief 계정을 주소와 따로 받아 합칠 때 특수문자가 든 비밀번호가 살아남는지 검사합니다.
 *
 * @details 손으로 "rtsp://id:p@ss@host"처럼 이어 붙이면 호스트가 'ss@host'로 잘린다.
 *          입력 화면에서 비밀번호를 가리려고 계정을 분리한 김에 이 파싱 문제도 같이 막는다.
 */
void checkCredentialComposition(const QString& configPath) {
    const QStringList addresses = {QStringLiteral("rtsp://10.0.0.9:554/0/media.smp"),
                                   QStringLiteral("rtsp://10.0.0.9:554/1/media.smp"),
                                   QStringLiteral("rtsp://10.0.0.9:554/2/media.smp"),
                                   QStringLiteral("rtsp://admin:plain@10.0.0.9:554/3/media.smp")};
    const QString password = QStringLiteral("p@ss:w/rd");
    check(ApplicationConfigWriter::appendArea(configPath, QStringLiteral("계정 구역"), addresses,
                                              QStringLiteral("veda"), password)
              .isEmpty(),
          "a zone with separately entered credentials must be appended");

    const ApplicationConfigLoadResult result = ApplicationConfigLoader::load();
    check(result.successful, "a config with encoded credentials must load back");
    if (!result.successful) {
        std::fprintf(stderr, "  load error: %s\n", qPrintable(result.error));
        return;
    }

    const QVector<int> channels = result.config.video.areas.last().channelIndexes;
    const QString composedUrl = result.config.video.streams.at(channels.first()).url;
    const QUrl composed(composedUrl);
    check(composed.host() == QStringLiteral("10.0.0.9"), "the host must survive a password containing '@'");
    check(composed.userName(QUrl::FullyDecoded) == QStringLiteral("veda"), "the user name must round trip");
    // GstRtspReceiver::applySourceProperties가 읽는 방식과 같게 확인한다
    check(composed.password(QUrl::FullyDecoded) == password, "the password must decode back to what was typed");
    check(!composedUrl.contains(password), "the stored url must keep the password percent-encoded, not raw");

    // 주소에 이미 적힌 계정이 우선한다 (카메라마다 계정이 다른 구성)
    const QUrl explicitUrl(result.config.video.streams.at(channels.last()).url);
    check(explicitUrl.userName(QUrl::FullyDecoded) == QStringLiteral("admin"),
          "credentials typed into the address must win over the zone account");
}

/** @brief 구역을 추가하면 채널·구역 상자가 함께 늘어나 그대로 다시 읽히는지 검사합니다. */
void checkAppendedZoneLoadsBack(const QString& configPath) {
    check(ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 3구역"), zoneUrls(9)).isEmpty(),
          "a valid zone must be appended");

    const ApplicationConfigLoadResult result = ApplicationConfigLoader::load();
    check(result.successful, "the appended config must load back");
    if (!result.successful) {
        std::fprintf(stderr, "  load error: %s\n", qPrintable(result.error));
        return;
    }

    const VideoRuntimeConfig& video = result.config.video;
    check(video.areas.size() == 3, "the new area must be visible to the loader");
    check(video.streams.size() == 3 * videoChannelsPerArea, "the new area must bring its own channels");
    check(video.areas.last().name == QStringLiteral("제 3구역"), "the entered name must survive the round trip");
    check(video.areas.last().channelIndexes == QVector<int>({8, 9, 10, 11}),
          "the new channels must continue the global numbering");
    check(result.config.mqtt.channelCount == 12, "the MQTT layer must widen to the new channels");

    // 새 구역 상자는 앞의 두 구역 간격만큼 떨어진 같은 크기여야 한다
    const DigitalTwinWorldConfig& world = result.config.digitalTwin.world;
    check(world.zoneCount() == 3, "a zone box must be added alongside the area");
    const QRectF second = world.zoneBounds(1);
    const QRectF third = world.zoneBounds(2);
    check(qFuzzyCompare(third.width(), second.width()) && qFuzzyCompare(third.height(), second.height()),
          "the new zone box must keep the calibrated size, so it stays square");
    check(qFuzzyCompare(third.left() - second.left(), second.left() - world.zoneBounds(0).left()),
          "the new zone box must repeat the spacing of the existing zones");
    check(third.left() >= second.right(), "the new zone box must not overlap the previous one");
    check(world.zoneIndexForWorldX(third.center().x()) == 2, "the new zone must own its own world coordinates");
}

/** @brief 도면이 그릴 수 있는 칸을 넘어서면 거절하는지 검사합니다. */
void checkCapacityCeiling(const QString& configPath) {
    for (int zoneNumber = ApplicationConfigLoader::load().config.video.areas.size() + 1;
         zoneNumber <= digitalTwinMaximumZoneCount; ++zoneNumber) {
        const QString error = ApplicationConfigWriter::appendArea(
            configPath, QStringLiteral("제 %1구역").arg(zoneNumber), zoneUrls(zoneNumber * videoChannelsPerArea - 3));
        check(error.isEmpty(), "every zone up to the drawing capacity must be accepted");
    }

    check(!ApplicationConfigWriter::appendArea(configPath, QStringLiteral("제 9구역"), zoneUrls(33)).isEmpty(),
          "a zone past the drawing capacity must be rejected");

    const ApplicationConfigLoadResult result = ApplicationConfigLoader::load();
    check(result.successful && result.config.video.areas.size() == digitalTwinMaximumZoneCount,
          "a full config must still load");
}
/**
 * @brief 이미 있는 구역을 고쳐 저장하면 그 구역만 바뀌는지 검사합니다.
 *
 * @details 편집 화면은 주소에서 계정을 떼어 따로 보여 주고 저장할 때 다시 합친다. 그 왕복에서
 *          비밀번호가 사라지거나 옆 구역이 휩쓸리면 카메라 연결이 통째로 끊기므로 같이 확인한다.
 */
void checkZoneUpdate(const QString& configPath) {
    const ApplicationConfigLoadResult before = ApplicationConfigLoader::load();
    check(before.successful && before.config.video.areas.size() >= 2, "the update check needs at least two zones");
    if (!before.successful) {
        return;
    }

    const QString untouchedUrl = before.config.video.streams.at(4).url;
    const QStringList addresses = {
        QStringLiteral("rtsp://10.9.9.9:554/a/media.smp"), QStringLiteral("rtsp://10.9.9.9:554/b/media.smp"),
        QStringLiteral("rtsp://10.9.9.9:554/c/media.smp"), QStringLiteral("rtsp://10.9.9.9:554/d/media.smp")};
    const QString password = QStringLiteral("n3w:p@ss");
    check(ApplicationConfigWriter::updateArea(configPath, 0, QStringLiteral("고친 구역"), addresses,
                                              QStringLiteral("operator"), password)
              .isEmpty(),
          "an existing zone must be updatable");

    const ApplicationConfigLoadResult after = ApplicationConfigLoader::load();
    check(after.successful, "the updated config must load back");
    if (!after.successful) {
        std::fprintf(stderr, "  load error: %s\n", qPrintable(after.error));
        return;
    }

    check(after.config.video.areas.size() == before.config.video.areas.size(), "updating must not add or drop zones");
    check(after.config.video.streams.size() == before.config.video.streams.size(),
          "updating must not add or drop channels");
    check(after.config.video.areas.first().name == QStringLiteral("고친 구역"), "the new name must be stored");
    check(after.config.video.areas.first().channelIndexes == before.config.video.areas.first().channelIndexes,
          "channel numbering must survive an update");

    const QUrl updated(after.config.video.streams.at(0).url);
    check(updated.host() == QStringLiteral("10.9.9.9"), "the new address must be stored");
    check(updated.userName(QUrl::FullyDecoded) == QStringLiteral("operator"), "the new account must be stored");
    check(updated.password(QUrl::FullyDecoded) == password, "the new password must round trip");
    check(after.config.video.streams.at(4).url == untouchedUrl, "the next zone must be left alone");

    check(!ApplicationConfigWriter::updateArea(configPath, 99, QStringLiteral("없는 구역"), addresses).isEmpty(),
          "an out-of-range zone must be rejected");
    check(!ApplicationConfigWriter::updateArea(configPath, 0, QStringLiteral("  "), addresses).isEmpty(),
          "an empty name must be rejected on update too");
}
}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);

    const QString configPath = prepareWorkingCopy();
    if (configPath.isEmpty()) {
        std::fprintf(stderr, "FAIL: could not prepare a working copy of the example config\n");
        return 1;
    }
    qputenv("VEDA_CONFIG_FILE", configPath.toUtf8());

    checkEmptyConfigLoads(configPath);
    if (!seedSecondZone(configPath)) {
        std::fprintf(stderr, "FAIL: could not seed the two-zone starting point\n");
        return 1;
    }

    checkRejectedInput(configPath);
    checkAppendedZoneLoadsBack(configPath);
    checkCredentialComposition(configPath);
    checkCapacityCeiling(configPath);
    checkZoneUpdate(configPath);

    QFile::remove(configPath);
    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::fprintf(stdout, "Config area writer checks passed\n");
    return 0;
}
