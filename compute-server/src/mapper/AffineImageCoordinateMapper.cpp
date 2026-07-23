#include "mapper/AffineImageCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Mapper";

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

void AffineImageCoordinateMapper::map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const {
    const std::size_t inputCount = objects.size();

    // in-place 필터링: 출력 개수는 항상 입력 이하이므로 별도 벡터를 새로 만들지 않고
    // objects 자체에서 통과하는 원소만 앞으로 당긴 뒤 resize()로 잘라냄
    // (resize()로 줄이는 건 재할당을 유발하지 않음) -> 이 함수의 힙 할당이 0이 됨
    std::size_t writeIdx = 0;
    for (std::size_t readIdx = 0; readIdx < objects.size(); ++readIdx) {
        auto& object = objects[readIdx];

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
        // touchesBorder/bottomTruncated 는 건드리지 않음: 이 단계는 blur 경로 전용이고
        // 경계 판정은 Metadata 좌표계 기준으로 파서가 이미 한 번만 수행함
        // (여기서 clamp 후 재판정하면 '앱 화면 기준 경계'라는 다른 의미가 섞임)

        if (writeIdx != readIdx)
            objects[writeIdx] = std::move(object);
        ++writeIdx;
    }
    objects.resize(writeIdx);

    if (inputCount > 0 && writeIdx == 0) {
        logError(kIface, "ch=" + std::to_string(channelId) + " 입력 blur 객체 " + std::to_string(inputCount) +
                             "개가 전부 필터링됨 (scale/offset 설정 확인 필요)");
    }
}
