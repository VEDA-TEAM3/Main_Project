#include "mapper/AffineImageCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

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
    domain::ChannelFrame result;
    result.utcTime = frame.utcTime;
    result.channelId = frame.channelId;
    result.objects.reserve(frame.objects.size());

    for (auto& object : frame.objects) {
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
        result.objects.push_back(std::move(object));
    }
    return result;
}
