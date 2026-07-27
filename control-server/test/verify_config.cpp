/**
 * @file    verify_config.cpp
 * @brief   AppConfig 하드웨어 채널/zone 범위 검증 하네스
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/AppConfig.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

/**
 * @brief 테스트별 JSON을 기록하고 종료 시 임시 파일을 정리
 */
class TempConfigFile {
public:
    TempConfigFile() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("veda-verify-config-" + std::to_string(suffix) + ".json");
    }

    ~TempConfigFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    bool write(const std::string& contents) const {
        std::ofstream output(path_, std::ios::trunc);
        output << contents;
        return output.good();
    }

    std::string path() const { return path_.string(); }

    TempConfigFile(const TempConfigFile&) = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;

private:
    std::filesystem::path path_;
};

}  // namespace

int main() {
    TempConfigFile file;

    const std::string zonesConfig = R"({
        "channelCount": 4,
        "zones": [
            {"zoneId": 0, "minX": 0.0, "maxX": 10.0, "minY": 0.0, "maxY": 10.0},
            {"zoneId": 7, "minX": 10.0, "maxX": 20.0, "minY": 0.0, "maxY": 10.0},
            {"zoneId": -1, "minX": 20.0, "maxX": 30.0, "minY": 0.0, "maxY": 10.0},
            {"zoneId": 0, "minX": 30.0, "maxX": 40.0, "minY": 0.0, "maxY": 10.0},
            {"zoneId": 1, "minX": 50.0, "maxX": 40.0, "minY": 0.0, "maxY": 10.0},
            {"zoneId": 2, "minX": 40.0, "maxX": 50.0, "minY": 0.0, "maxY": 10.0}
        ]
    })";

    check(file.write(zonesConfig), "zone 검증용 임시 설정 파일 기록");
    const AppConfig zones = AppConfig::load(file.path());
    check(zones.channelCount == 4, "유효한 channelCount는 유지");
    check(zones.zones.size() == 2, "범위 밖·중복·잘못된 AABB zone은 제거");
    if (zones.zones.size() == 2) {
        check(zones.zones[0].zoneId == 0 && zones.zones[1].zoneId == 2, "유효한 zone과 선언 순서를 유지");
    }

    const std::string channelUpperBoundConfig = R"({
        "channelCount": 999,
        "zones": [
            {"zoneId": 255, "minX": 0.0, "maxX": 1.0, "minY": 0.0, "maxY": 1.0},
            {"zoneId": 256, "minX": 1.0, "maxX": 2.0, "minY": 0.0, "maxY": 1.0}
        ]
    })";
    check(file.write(channelUpperBoundConfig), "상한 검증용 임시 설정 파일 기록");
    const AppConfig tooManyChannels = AppConfig::load(file.path());
    check(tooManyChannels.channelCount == 256, "channelCount 999는 uint8_t 채널 개수 상한 256으로 보정");
    check(tooManyChannels.zones.size() == 1 && tooManyChannels.zones[0].zoneId == 255,
          "zoneId 255는 유지하고 256은 제거");

    check(file.write(R"({"channelCount": -5})"), "하한 검증용 임시 설정 파일 기록");
    const AppConfig negativeChannels = AppConfig::load(file.path());
    check(negativeChannels.channelCount == 1, "음수 channelCount는 최소값 1로 보정");

    std::printf("\nPASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
