#include "transform/HomographyTransform.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Transform";

/// @brief 부호 정규화의 기준점: 화면 하단 중앙 (아래로 기울어진 CCTV 라면 항상 카메라 앞쪽 지면)
constexpr double kAnchorU = 0.5;
constexpr double kAnchorV = 1.0;

/// @brief 행렬이 (스칼라배를 감안해) 단위행렬과 같은지 — 캘리브레이션 누락 감지용
bool isIdentityLike(const std::array<double, 9>& m) {
    if (std::abs(m[8]) < 1e-12)
        return false;
    const double s = m[8];
    const std::array<double, 9> expected{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (std::abs(m[i] / s - expected[i]) > 1e-9)
            return false;
    }
    return true;
}

double determinant(const std::array<double, 9>& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

}  // namespace

HomographyTransform::HomographyTransform(std::array<double, 9> matrix, const Options& options)
    : matrix_(matrix), options_(options) {
    for (double value : matrix_) {
        if (!std::isfinite(value))
            throw std::invalid_argument("homography matrix must contain only finite values");
    }

    // 1) 픽셀 좌표계 행렬이면 정규화 좌표계로 환산 (생성 시 1회, 런타임 비용 0)
    //    H_norm = H_pixel * diag(W, H, 1)  ->  열 0 에 W, 열 1 에 H 를 곱함
    if (options_.pixelSpace) {
        if (!(options_.imageWidth > 0.0) || !(options_.imageHeight > 0.0)) {
            throw std::invalid_argument(
                "homographySpace=\"pixel\" requires positive imageWidth/imageHeight in config.json");
        }
        for (std::size_t row = 0; row < 3; ++row) {
            matrix_[row * 3 + 0] *= options_.imageWidth;
            matrix_[row * 3 + 1] *= options_.imageHeight;
        }
        logSuccess(kIface, "픽셀 좌표계 호모그래피를 정규화 좌표계로 환산함 (W=" + std::to_string(options_.imageWidth) +
                               ", H=" + std::to_string(options_.imageHeight) + ")");
    }

    if (std::abs(determinant(matrix_)) < 1e-12)
        throw std::invalid_argument("homography matrix is singular (determinant ~= 0)");

    // 2) 부호 정규화: 호모그래피는 스칼라배 불변이므로 전체에 -1 을 곱해도 사상 결과는 동일함
    //    기준점(화면 하단 중앙)의 분모가 양수가 되도록 맞춰두면, 이후 toLocal()에서는
    //    'denominator > 0' 하나로 지평선 너머를 판별할 수 있음
    const double anchorDenominator = matrix_[6] * kAnchorU + matrix_[7] * kAnchorV + matrix_[8];
    if (std::abs(anchorDenominator) < kMinDenominator) {
        // 기준점 자체가 지평선 위에 놓임 -> 부호의 의미를 정할 수 없으므로 검사를 끔
        horizonCheckEnabled_ = false;
        logError(kIface,
                 "화면 하단 중앙이 지평선과 겹칩니다 (분모≈0) - 지평선 검사를 비활성화하고 분모 크기만 검사합니다. "
                 "호모그래피 캘리브레이션을 다시 확인하세요");
    } else if (anchorDenominator < 0.0) {
        for (double& value : matrix_)
            value = -value;
    }

    // 3) 캘리브레이션 누락 감지: 단위행렬이면 '정규화 이미지 좌표를 미터라고 주장'하는 상태
    if (isIdentityLike(matrix_)) {
        logError(kIface,
                 "homography 가 단위행렬입니다 - 월드 좌표가 사실상 정규화 이미지 좌표([0,1])로 나갑니다. "
                 "config.json 의 homography/homographySpace 설정을 확인하세요");
    }

    if (options_.boundsEnabled) {
        logSuccess(kIface, "로컬 좌표 범위 검사 활성화 x=[" + std::to_string(options_.minX) + ", " +
                               std::to_string(options_.maxX) + "], y=[" + std::to_string(options_.minY) + ", " +
                               std::to_string(options_.maxY) + "]");
    }
}

std::optional<veda::LocalPoint> HomographyTransform::toLocal(const domain::ImagePoint& p) {
    const double denominator = matrix_[6] * p.u + matrix_[7] * p.v + matrix_[8];

    // 지평선 판정: 부호 정규화가 된 경우 denominator <= 0 은 '지평선 위/너머'를 뜻함
    // (부호가 뒤집힌 채로 통과시키면 반대편으로 반사된 그럴듯한 좌표가 나와 팬텀 객체가 됨)
    const bool denominatorBad =
        horizonCheckEnabled_ ? (denominator <= kMinDenominator) : (std::abs(denominator) < kMinDenominator);
    if (denominatorBad) {
        logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) +
                   " 지면점이 지평선 위/근처 (분모=" + std::to_string(denominator) + ") - 변환 불가");
        return std::nullopt;
    }

    const veda::LocalPoint world{(matrix_[0] * p.u + matrix_[1] * p.v + matrix_[2]) / denominator,
                                 (matrix_[3] * p.u + matrix_[4] * p.v + matrix_[5]) / denominator};

    if (!std::isfinite(world.x) || !std::isfinite(world.y)) {
        logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) + " 변환 결과가 유한하지 않음");
        return std::nullopt;
    }

    if (options_.boundsEnabled &&
        (world.x < options_.minX || world.x > options_.maxX || world.y < options_.minY || world.y > options_.maxY)) {
        logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) + " -> (" + std::to_string(world.x) +
                   ", " + std::to_string(world.y) + ")m 가 로컬 좌표 범위를 벗어남 - 폐기");
        return std::nullopt;
    }

    return world;
}

void HomographyTransform::logFailure(const std::string& message) {
    // 지평선 근처 객체는 프레임마다 연속으로 실패하므로 매번 찍으면 로그가 도배됨
    ++failureCount_;
    if (failureCount_ == 1 || failureCount_ % 100 == 0) {
        logError(kIface, message + " (누적 " + std::to_string(failureCount_) + "건)");
    }
}
