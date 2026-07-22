#include "transform/HomographyTransform.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Transform";
}  // namespace

HomographyTransform::HomographyTransform(std::array<double, 9> matrix) : matrix_(matrix) {
    for (double value : matrix_) {
        if (!std::isfinite(value))
            throw std::invalid_argument("homography matrix must contain only finite values");
    }
}

std::optional<veda::WorldPoint> HomographyTransform::toWorld(const domain::ImagePoint& p) {
    const double denominator = matrix_[6] * p.u + matrix_[7] * p.v + matrix_[8];
    if (std::abs(denominator) < 1e-12) {
        logError(kIface, "u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) +
                              " 호모그래피 분모가 0에 근접 - 변환 불가");
        return std::nullopt;
    }

    return veda::WorldPoint{(matrix_[0] * p.u + matrix_[1] * p.v + matrix_[2]) / denominator,
                            (matrix_[3] * p.u + matrix_[4] * p.v + matrix_[5]) / denominator};
}