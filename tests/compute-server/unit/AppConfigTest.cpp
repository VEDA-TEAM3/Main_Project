/**
 * @file    AppConfigTest.cpp
 * @brief   compute-server 설정 파일의 경계값 및 안전한 fallback 회귀 테스트
 */

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "core/AppConfig.h"

namespace {

class TempConfig {
public:
    explicit TempConfig(const std::string& content) {
        static std::atomic<unsigned int> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("veda-compute-config-test-" + std::to_string(sequence.fetch_add(1)) + ".json");
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~TempConfig() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

AppConfig loadJson(const nlohmann::json& json) {
    const TempConfig file(json.dump());
    return AppConfig::load(file.path());
}

}  // namespace

TEST(ComputeAppConfigTest, MissingOrMalformedFileUsesDefaultsWithoutThrowing) {
    const AppConfig defaults;
    EXPECT_NO_THROW({
        const auto missing = AppConfig::load("/tmp/veda-config-that-does-not-exist.json");
        EXPECT_EQ(missing.rtspPort, defaults.rtspPort);
        EXPECT_EQ(missing.blurBoxScale, defaults.blurBoxScale);
    });

    const TempConfig malformed("{not-json");
    EXPECT_NO_THROW({
        const auto config = AppConfig::load(malformed.path());
        EXPECT_EQ(config.mqttPort, defaults.mqttPort);
        EXPECT_EQ(config.riskEdgePolicy, defaults.riskEdgePolicy);
    });
}

TEST(ComputeAppConfigTest, IntegerPoliciesAreClampedAtOperationalBounds) {
    const auto config = loadJson({
        {"rtspPort", 0},
        {"mqttPort", 70000},
        {"rtspConnectTimeoutSec", 0},
        {"rtspRecvTimeoutSec", AppConfig::kMaxRtspPolicySeconds + 1},
        {"rtspReadBufBytes", 1},
        {"rtspSocketRecvBufBytes", AppConfig::kMaxSocketBufferBytes + 1},
        {"sourceRingCapacity", AppConfig::kMaxSourceRingCapacity + 1},
        {"mqttBlurMaxQueueSize", 0},
        {"mqttTopViewMaxQueueSize", AppConfig::kMaxMqttQueueSize + 1},
    });

    EXPECT_EQ(config.rtspPort, 1);
    EXPECT_EQ(config.mqttPort, 65535);
    EXPECT_EQ(config.rtspConnectTimeoutSec, 1);
    EXPECT_EQ(config.rtspRecvTimeoutSec, AppConfig::kMaxRtspPolicySeconds);
    EXPECT_EQ(config.rtspReadBufBytes, 4096);
    EXPECT_EQ(config.rtspSocketRecvBufBytes, AppConfig::kMaxSocketBufferBytes);
    EXPECT_EQ(config.sourceRingCapacity, AppConfig::kMaxSourceRingCapacity);
    EXPECT_EQ(config.mqttBlurMaxQueueSize, 1);
    EXPECT_EQ(config.mqttTopViewMaxQueueSize, AppConfig::kMaxMqttQueueSize);
}

TEST(ComputeAppConfigTest, ExactOperationalBoundsArePreserved) {
    const auto config = loadJson({
        {"rtspPort", 65535},
        {"rtspConnectTimeoutSec", AppConfig::kMaxRtspPolicySeconds},
        {"sourceRingCapacity", 1},
        {"mqttBlurMaxQueueSize", AppConfig::kMaxMqttQueueSize},
        {"edgeEpsilon", 0.1},
        {"sanitizerIouThresh", 0.0},
        {"sanitizerContainThresh", 1.0},
        {"blurBoxScale", AppConfig::kMaxBlurBoxScale},
    });

    EXPECT_EQ(config.rtspPort, 65535);
    EXPECT_EQ(config.rtspConnectTimeoutSec, AppConfig::kMaxRtspPolicySeconds);
    EXPECT_EQ(config.sourceRingCapacity, 1);
    EXPECT_EQ(config.mqttBlurMaxQueueSize, AppConfig::kMaxMqttQueueSize);
    EXPECT_DOUBLE_EQ(config.edgeEpsilon, 0.1);
    EXPECT_DOUBLE_EQ(config.sanitizerIouThresh, 0.0);
    EXPECT_DOUBLE_EQ(config.sanitizerContainThresh, 1.0);
    EXPECT_DOUBLE_EQ(config.blurBoxScale, AppConfig::kMaxBlurBoxScale);
}

TEST(ComputeAppConfigTest, BackoffMaximumCannotBeLowerThanInitialValue) {
    const auto config = loadJson({
        {"rtspReconnectBackoffInitialSec", 30},
        {"rtspReconnectBackoffMaxSec", 5},
        {"mqttReconnectDelaySec", 20},
        {"mqttReconnectDelayMaxSec", 3},
    });

    EXPECT_EQ(config.rtspReconnectBackoffMaxSec, 30);
    EXPECT_EQ(config.mqttReconnectDelayMaxSec, 20);
}

TEST(ComputeAppConfigTest, UnsafeTextAndLogPathsAreRejected) {
    const auto config = loadJson({
        {"rtspSetupUri", "rtsp://camera/stream\r\nInjected: true"},
        {"mqttClientId", std::string(256, 'x')},
        {"logFileName", "../outside.csv"},
    });

    EXPECT_TRUE(config.rtspSetupUri.empty());
    EXPECT_TRUE(config.mqttClientId.empty());
    EXPECT_EQ(config.logFileName, "veda.csv");
}

TEST(ComputeAppConfigTest, InvalidEnumsAndLocalBoundsFallBackSafely) {
    const auto config = loadJson({
        {"logLevel", "verbose"},
        {"homographySpace", "world"},
        {"riskEdgePolicy", "sometimes"},
        {"localBoundsEnabled", true},
        {"localMinX", 10.0},
        {"localMaxX", 10.0},
        {"localMinY", -1.0},
        {"localMaxY", 1.0},
    });

    EXPECT_EQ(config.logLevel, "info");
    EXPECT_EQ(config.homographySpace, "normalized");
    EXPECT_EQ(config.riskEdgePolicy, "dropBottomTruncated");
    EXPECT_FALSE(config.localBoundsEnabled);
}

TEST(ComputeAppConfigTest, InvalidHomographyLengthKeepsIdentityDefault) {
    const AppConfig defaults;
    const auto config = loadJson({{"homography", {1.0, 2.0, 3.0}}});

    EXPECT_EQ(config.homography, defaults.homography);
}

TEST(ComputeAppConfigTest, NonFiniteFloatingPointPoliciesAreClamped) {
    double value = std::numeric_limits<double>::quiet_NaN();
    AppConfig::clampRange(value, 1.0, 2.0, "testValue");
    EXPECT_DOUBLE_EQ(value, 1.0);

    value = std::numeric_limits<double>::infinity();
    AppConfig::clampRange(value, -1.0, 1.0, "testValue");
    EXPECT_DOUBLE_EQ(value, -1.0);
}

TEST(ComputeAppConfigTest, OversizedConfigFileIsRejectedBeforeParsing) {
    const AppConfig defaults;
    const TempConfig oversized(std::string(AppConfig::kMaxConfigFileBytes + 1, ' '));

    const auto config = AppConfig::load(oversized.path());

    EXPECT_EQ(config.rtspPort, defaults.rtspPort);
    EXPECT_EQ(config.mqttPort, defaults.mqttPort);
}
