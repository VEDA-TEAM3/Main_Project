#include "video/VideoPreprocessLut.h"

#include <opencv2/core.hpp>

namespace {
/**
 * @brief        실수를 내림합니다.
 * @param value  내림할 값
 * @return       value 이하의 가장 큰 정수
 *
 * @details std::floor 대신 캐스팅으로 처리합니다. 이 클라이언트는 GStreamer를 링크하는 순간
 *          여러 libm 함수가 로드 단계에서 죽습니다(BlurProcessor의 boundedFloorPixel이
 *          손으로 짜여 있는 것과 같은 이유입니다). 캐스팅은 0을 향해 자르므로 음수에서
 *          한 칸 보정합니다.
 */
int floorToInt(double value) {
    int truncated = static_cast<int>(value);
    if (static_cast<double>(truncated) > value) {
        --truncated;
    }
    return truncated;
}

int clampToByte(int value) { return value < 0 ? 0 : (value > 255 ? 255 : value); }

/**
 * @brief        짝수 반올림으로 0~255에 맞춥니다. videobalance가 쓰는 방식입니다.
 * @param value  반올림할 값
 * @return       0 이상 255 이하의 정수
 *
 * @details 정확히 .5인 값을 짝수 쪽으로 보냅니다. 실측으로 확인한 차이입니다 - 대비 1.5에서
 *          Y=7이 2.5가 되는데 videobalance는 2를 냅니다. 반올림 방식이 어긋나면 프리셋 화면이
 *          미묘하게 달라지고, 감마가 그 1 차이를 더 벌립니다.
 */
int roundHalfToEvenByte(double value) {
    const int lower = floorToInt(value);
    const double fraction = value - static_cast<double>(lower);
    if (fraction > 0.5) {
        return clampToByte(lower + 1);
    }
    if (fraction < 0.5) {
        return clampToByte(lower);
    }
    return clampToByte(lower % 2 == 0 ? lower : lower + 1);
}

/**
 * @brief        올림 반올림으로 0~255에 맞춥니다. gamma 요소가 쓰는 방식입니다.
 * @param value  반올림할 값
 * @return       0 이상 255 이하의 정수
 *
 * @details 두 요소의 반올림이 서로 다릅니다. gamma 쪽은 .5를 위로 올립니다.
 */
int roundHalfUpByte(double value) { return clampToByte(floorToInt(value + 0.5)); }
}  // namespace

bool isNeutralVideoPreprocessing(const VideoPreprocessingSettings& settings) {
    return !settings.enabled || (settings.brightness == 0 && settings.contrast == 1.0 && settings.gamma == 1.0);
}

/**
 * @details 두 단계의 수식은 GStreamer 요소가 실제로 내는 값에서 역산해 확인했습니다.
 *          - 밝기·대비는 흑색 기준 **16을 축으로** 돕니다(videobalance). 0이 아니라 16이라
 *            한계 범위(16~235) 영상의 검은 부분이 밀리지 않습니다
 *          - 감마는 **0~255 전체**를 씁니다(gamma 요소). 두 요소의 기준이 서로 다른데,
 *            그 차이까지 그대로 재현해야 화면이 예전과 같습니다
 *          두 요소는 각자 중립값에서 passthrough로 내려가므로, 합성할 때도 중립 단계는
 *          항등으로 두어야 값이 어긋나지 않습니다.
 *
 *          지수는 **cv::pow**로 계산합니다. std::pow는 이 클라이언트에서 쓸 수 없습니다 -
 *          GStreamer를 링크하면 pseudo relocation으로 실행 즉시 죽습니다. cv::pow는 계산이
 *          libopencv_core DLL 안에서 끝나 그 참조가 우리 오브젝트에 생기지 않습니다.
 */
VideoPreprocessLut buildVideoPreprocessLut(const VideoPreprocessingSettings& settings) {
    VideoPreprocessLut lut{};
    for (int input = 0; input < 256; ++input) {
        lut[static_cast<size_t>(input)] = static_cast<unsigned char>(input);
    }
    if (isNeutralVideoPreprocessing(settings)) {
        return lut;
    }

    // 1단계 videobalance: 클라이언트의 -100~100 눈금을 요소의 -1~1로 되돌린다
    const bool balanceNeutral = settings.brightness == 0 && settings.contrast == 1.0;
    if (!balanceNeutral) {
        const double brightness = static_cast<double>(settings.brightness) / 100.0;
        for (int input = 0; input < 256; ++input) {
            const double value = 16.0 + (static_cast<double>(input) - 16.0) * settings.contrast + brightness * 255.0;
            lut[static_cast<size_t>(input)] = static_cast<unsigned char>(roundHalfToEvenByte(value));
        }
    }

    // 2단계 gamma: 1단계 결과를 그대로 이어받는다
    if (settings.gamma != 1.0) {
        cv::Mat normalized(1, 256, CV_64F);
        auto* row = normalized.ptr<double>(0);
        for (int input = 0; input < 256; ++input) {
            row[input] = static_cast<double>(lut[static_cast<size_t>(input)]) / 255.0;
        }

        cv::Mat corrected;
        cv::pow(normalized, 1.0 / settings.gamma, corrected);

        const auto* correctedRow = corrected.ptr<double>(0);
        for (int input = 0; input < 256; ++input) {
            lut[static_cast<size_t>(input)] = static_cast<unsigned char>(roundHalfUpByte(correctedRow[input] * 255.0));
        }
    }

    return lut;
}
