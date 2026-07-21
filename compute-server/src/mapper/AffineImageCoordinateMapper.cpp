#include "mapper/AffineImageCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "log/Logger.h"

namespace {

constexpr const char* kIface = "Mapper";
constexpr double kEdgeEpsilon = 0.002;

bool isFinite(double value) { return std::isfinite(value); }

bool isVisible(const domain::NormBox& box) { return box.r > 0.0 && box.l < 1.0 && box.b > 0.0 && box.t < 1.0; }

void clampToOutput(domain::NormBox& box) {
    box.l = std::clamp(box.l, 0.0, 1.0);
    box.r = std::clamp(box.r, 0.0, 1.0);
    box.t = std::clamp(box.t, 0.0, 1.0);
    box.b = std::clamp(box.b, 0.0, 1.0);
}

}  // namespace

AffineImageCoordinateMapper::AffineImageCoordinateMapper(double scaleX, double scaleY, double offsetX, double offsetY)
    : scaleX_(scaleX), scaleY_(scaleY), offsetX_(offsetX), offsetY_(offsetY) {
    if (!isFinite(scaleX_) || !isFinite(scaleY_) || !isFinite(offsetX_) || !isFinite(offsetY_) || scaleX_ <= 0.0 ||
        scaleY_ <= 0.0)
        throw std::invalid_argument("image coordinate mapper requires finite positive scales");
}

domain::ChannelFrame AffineImageCoordinateMapper::map(domain::ChannelFrame frame) const {
    const std::size_t inputCount = frame.objects.size();

    // in-place 필터링: 출력 개수는 항상 입력 이하이므로 별도 벡터를 새로 만들지 않고
    // frame.objects 자체에서 통과하는 원소만 앞으로 당긴 뒤 resize()로 잘라냄
    // (resize()로 줄이는 건 재할당을 유발하지 않음) -> 이 함수의 힙 할당이 0이 됨
    std::size_t writeIdx = 0;
    for (std::size_t readIdx = 0; readIdx < frame.objects.size(); ++readIdx) {
        auto& object = frame.objects[readIdx];

        domain::NormBox mapped;
        mapped.l = object.box.l * scaleX_ + offsetX_;
        mapped.r = object.box.r * scaleX_ + offsetX_;
        mapped.t = object.box.t * scaleY_ + offsetY_;
        mapped.b = object.box.b * scaleY_ + offsetY_;

        if (!isFinite(mapped.l) || !isFinite(mapped.r) || !isFinite(mapped.t) || !isFinite(mapped.b) ||
            !isVisible(mapped))
            continue;

        clampToOutput(mapped);
        if (mapped.r <= mapped.l || mapped.b <= mapped.t)
            continue;

        object.box = mapped;
        object.touchesBorder = mapped.l <= kEdgeEpsilon || mapped.r >= 1.0 - kEdgeEpsilon || mapped.t <= kEdgeEpsilon ||
                               mapped.b >= 1.0 - kEdgeEpsilon;

        if (writeIdx != readIdx)
            frame.objects[writeIdx] = std::move(object);
        ++writeIdx;
    }
    frame.objects.resize(writeIdx);

    if (inputCount > 0 && writeIdx == 0) {
        logError(kIface, "ch=" + std::to_string(frame.channelId) + " 입력 객체 " + std::to_string(inputCount) +
                              "개가 전부 필터링됨 (scale/offset 설정 확인 필요)");
    }
    return frame;
}
