#include "transform/HomographyTransform.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Transform";

/// @brief 부호 정규화의 기준점: 화면 하단 중앙 (아래로 기울어진 CCTV 라면 항상 카메라 앞쪽 지면)
constexpr double kAnchorU = 0.5;
constexpr double kAnchorV = 1.0;

/// @brief max(|h_i|)=1 로 정규화된 3x3 행렬의 특이성 판정 임계값
constexpr double kMinNormalizedDeterminant = 1e-12;

/// @brief 행렬이 (스칼라배를 감안해) 단위행렬과 같은지 — 캘리브레이션 누락 감지용
bool isIdentityLike(const std::array<double, 9>& m) {
    if (std::abs(m[8]) < 1e-12) {
        return false;
    }
    const double s = m[8];
    const std::array<double, 9> expected{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (std::abs(m[i] / s - expected[i]) > 1e-9) {
            return false;
        }
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
        if (!std::isfinite(value)) {
            throw std::invalid_argument("homography matrix must contain only finite values");
        }
    }

    // 1) 픽셀 좌표계 행렬이면 정규화 좌표계로 환산 (생성 시 1회, 런타임 비용 0)
    //    H_norm = H_pixel * diag(W, H, 1)  ->  열 0 에 W, 열 1 에 H 를 곱함
    if (options_.pixelSpace) {
        if (!std::isfinite(options_.imageWidth) || !std::isfinite(options_.imageHeight) ||
            !(options_.imageWidth > 0.0) || !(options_.imageHeight > 0.0)) {
            throw std::invalid_argument(
                "homographySpace=\"pixel\" requires finite positive imageWidth/imageHeight in config.json");
        }
        for (std::size_t row = 0; row < 3; ++row) {
            matrix_[row * 3 + 0] *= options_.imageWidth;
            matrix_[row * 3 + 1] *= options_.imageHeight;
        }
        for (double value : matrix_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("homography matrix overflowed during pixel-to-normalized conversion");
            }
        }
        logSuccess(kIface, "픽셀 좌표계 호모그래피를 정규화 좌표계로 환산함 (W=" + std::to_string(options_.imageWidth) +
                               ", H=" + std::to_string(options_.imageHeight) + ")");
    }

    if (options_.boundsEnabled) {
        if (!std::isfinite(options_.minX) || !std::isfinite(options_.maxX) || !std::isfinite(options_.minY) ||
            !std::isfinite(options_.maxY)) {
            throw std::invalid_argument("local coordinate bounds must contain only finite values");
        }
        if (!(options_.minX < options_.maxX) || !(options_.minY < options_.maxY)) {
            throw std::invalid_argument("local coordinate bounds require minX < maxX and minY < maxY");
        }
    }

    // 2) 호모그래피는 0이 아닌 임의 스칼라배가 같은 사상이므로 행렬 크기를 먼저 정규화한다.
    //    이후 determinant/분모 임계값은 H, 1e-6*H, 1e6*H 에서 동일하게 동작한다.
    double maxAbsValue = 0.0;
    for (double value : matrix_) {
        maxAbsValue = std::max(maxAbsValue, std::abs(value));
    }
    if (!(maxAbsValue > 0.0) || !std::isfinite(maxAbsValue)) {
        throw std::invalid_argument("homography matrix must have a finite non-zero scale");
    }
    for (double& value : matrix_) {
        value /= maxAbsValue;
    }

    const double normalizedDeterminant = determinant(matrix_);
    if (!std::isfinite(normalizedDeterminant) || std::abs(normalizedDeterminant) < kMinNormalizedDeterminant) {
        throw std::invalid_argument("homography matrix is singular (determinant ~= 0)");
    }

    // 3) 부호 정규화: 호모그래피는 스칼라배 불변이므로 전체에 -1 을 곱해도 사상 결과는 동일함
    //    기준점(화면 하단 중앙)의 분모가 양수가 되도록 맞춰두면, 이후 toLocal()에서는
    //    'denominator > 0' 하나로 지평선 너머를 판별할 수 있음
    const double anchorDenominator = matrix_[6] * kAnchorU + matrix_[7] * kAnchorV + matrix_[8];
    if (!std::isfinite(anchorDenominator) || std::abs(anchorDenominator) <= kMinDenominator) {
        throw std::invalid_argument(
            "homography horizon crosses the bottom-center anchor - recalibrate instead of disabling horizon checks");
    }
    if (anchorDenominator < 0.0) {
        for (double& value : matrix_) {
            value = -value;
        }
    }

    // 4) 캘리브레이션 누락은 미터 기반 위험 판정을 조용히 무력화하므로 fail-fast
    if (isIdentityLike(matrix_)) {
        throw std::invalid_argument(
            "homography matrix is identity - normalized image coordinates cannot be emitted as local metres");
    }

    if (options_.boundsEnabled) {
        logSuccess(kIface, "로컬 좌표 범위 검사 활성화 x=[" + std::to_string(options_.minX) + ", " +
                               std::to_string(options_.maxX) + "], y=[" + std::to_string(options_.minY) + ", " +
                               std::to_string(options_.maxY) + "]");
    }
}

std::optional<veda::LocalPoint> HomographyTransform::toLocal(const domain::ImagePoint& p) {
    if (!std::isfinite(p.u) || !std::isfinite(p.v) || p.u < 0.0 || p.u > 1.0 || p.v < 0.0 || p.v > 1.0) {
        if (shouldLogFailure()) {
            logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) +
                       " 정규화 이미지 좌표 범위([0,1])를 벗어남 - 변환 불가");
        }
        return std::nullopt;
    }

    const double denominator = matrix_[6] * p.u + matrix_[7] * p.v + matrix_[8];

    // 지평선 판정: 생성자에서 부호와 크기가 정규화되므로 denominator <= 0 은 '지평선 위/너머'를 뜻함
    // (부호가 뒤집힌 채로 통과시키면 반대편으로 반사된 그럴듯한 좌표가 나와 팬텀 객체가 됨)
    if (!std::isfinite(denominator) || denominator <= kMinDenominator) {
        // 문자열 조립은 rate-limit 을 통과했을 때만 (억제될 로그의 힙 할당 제거)
        if (shouldLogFailure()) {
            logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) +
                       " 지면점이 지평선 위/근처 (분모=" + std::to_string(denominator) + ") - 변환 불가");
        }
        return std::nullopt;
    }

    const veda::LocalPoint local{(matrix_[0] * p.u + matrix_[1] * p.v + matrix_[2]) / denominator,
                                 (matrix_[3] * p.u + matrix_[4] * p.v + matrix_[5]) / denominator};

    if (!std::isfinite(local.x) || !std::isfinite(local.y)) {
        if (shouldLogFailure()) {
            logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) + " 변환 결과가 유한하지 않음");
        }
        return std::nullopt;
    }

    if (options_.boundsEnabled &&
        (local.x < options_.minX || local.x > options_.maxX || local.y < options_.minY || local.y > options_.maxY)) {
        if (shouldLogFailure()) {
            logFailure("u=" + std::to_string(p.u) + ", v=" + std::to_string(p.v) + " -> (" + std::to_string(local.x) +
                       ", " + std::to_string(local.y) + ")m 가 로컬 좌표 범위를 벗어남 - 폐기");
        }
        return std::nullopt;
    }

    return local;
}

bool HomographyTransform::shouldLogFailure() noexcept {
    // 지평선 근처 객체는 프레임마다 연속으로 실패하므로 매번 찍으면 로그가 도배됨
    // -> 누적 카운트는 항상 올리되(진단용), 실제 기록은 첫 건과 100건마다만
    ++failureCount_;

    // Error 레벨 자체가 꺼져 있으면 조립도 기록도 불필요
    if (!isLogEnabled(LogLevel::Error)) {
        return false;
    }
    return failureCount_ == 1 || failureCount_ % 100 == 0;
}

void HomographyTransform::logFailure(const std::string& message) const {
    logError(kIface, message + " (누적 " + std::to_string(failureCount_) + "건)");
}
