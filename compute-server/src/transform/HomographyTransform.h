#pragma once

/**
 * @file    HomographyTransform.h
 * @brief   정규화 이미지 좌표 → 카메라 로컬 지면 좌표(m) (Homography)
 */

#include <array>
#include <cstdint>
#include <string>

#include "interfaces/ICoordinateTransform.h"

/**
 * @brief   평면 호모그래피로 지면점을 카메라 로컬 지면 좌표(m)로 사상
 *
 * @details
 * toLocal()가 받는 점은 언제나 [0,1] 정규화 좌표(좌상단 원점)임
 * 반면 캘리브레이션 도구(OpenCV findHomography 등)가 뱉는 행렬은 보통 픽셀 좌표계 기준이라
 * 그대로 넣으면 h6/h7 분모 항의 스케일이 어긋나 예외 없이 완전히 틀린 좌표가 나옴
 * → Options::pixelSpace로 입력 좌표계를 명시하면 생성자가 한 번만 환산해둠
 *    (H_norm = H_pixel * diag(W, H, 1) → 열 0에 W, 열 1에 H 를 곱하는 것과 동일)
 *    런타임 경로에는 분기도 곱셈도 추가되지 않음
 *
 * @note [ 지평선 처리 ]
 * 분모 = h6*u + h7*v + h8 이 0 이 되는 직선이 곧 지평선임
 * 지평선 너머의 점은 분모 부호가 뒤집혀, 크기는 멀쩡하지만 반대편으로 반사된
 * 그럴듯한 좌표를 내놓음 → 팬텀 객체가 엉뚱한 위치에 발행됨
 * 호모그래피는 스칼라배 불변이므로, 생성자에서 화면 하단 중앙(0.5, 1.0)의 분모가
 * 양수가 되도록 전체 부호를 정규화해두면 이후엔 분모 > 0 검사만으로 지평선 너머를 걸러낼 수 있음
 * (하단 중앙을 기준점으로 삼는 이유: 아래로 기울어진 CCTV 라면 화면 맨 아래 지면은
 *  반드시 카메라 앞쪽이므로. 좌상단(0,0)은 지평선 위일 수 있어 기준으로 부적합)
 */
class HomographyTransform final : public ICoordinateTransform {
public:
    /**
     * @brief 행렬 해석 방식과 사후 검증 범위를 담는 설정
     */
    struct Options {
        /// @brief true면 matrix를 픽셀 좌표계 기준으로 보고 생성자에서 정규화 좌표계로 환산
        bool pixelSpace = false;

        /// @brief pixelSpace일 때 캘리브레이션에 사용한 이미지 해상도
        double imageWidth = 0.0;
        double imageHeight = 0.0;

        /// @brief 카메라 로컬 좌표 유효 범위 검사 (물리적으로 불가능한 발산값을 버림)
        bool boundsEnabled = false;
        double minX = 0.0;
        double maxX = 0.0;
        double minY = 0.0;
        double maxY = 0.0;
    };

    /**
     * @param   matrix  row-major 3x3 호모그래피 (h0..h2 / h3..h5 / h6..h8)
     * @param   options 입력 좌표계 및 검증 설정
     *
     * @throws  std::invalid_argument 유한하지 않은 값, 잘못된 해상도/범위,
     *                                특이 행렬, 지평선 기준점 오류 또는 단위행렬인 경우
     */
    HomographyTransform(std::array<double, 9> matrix, const Options& options);

    /// @brief 기본 설정(정규화 좌표계, 범위 검사 없음)
    explicit HomographyTransform(std::array<double, 9> matrix) : HomographyTransform(matrix, Options{}) {}

    std::optional<veda::LocalPoint> toLocal(const domain::ImagePoint& p) override;

private:
    /**
     * @brief   이번 변환 실패를 실제로 기록할 차례인지 판정 (rate-limit)
     * @details 실패 카운터를 올리고, 로그를 남길 차례(첫 건 또는 100건마다)일 때만 true
     *
     * @warning [ 문자열 조립은 반드시 이 함수가 true일 때만 할 것 ]
     * 예전에는 호출부가 logFailure("u=" + std::to_string(...) + ...) 형태였는데,
     * 인자는 함수 진입 이전에 평가되므로 rate-limit으로 억제될 99% 의 로그까지
     * 매번 문자열을 조립(= 힙 할당)했음
     * 지평선 근처 객체는 프레임마다 연속으로 실패하므로 이 비용이 객체당/프레임당 반복됨
     * → 판정(이 함수)과 기록(logFailure)을 분리해, 억제될 로그는 조립조차 하지 않음
     *    (ContainmentSanitizer / ParentBasedRouter 의 isLogEnabled() 가드와 같은 패턴)
     */
    bool shouldLogFailure() noexcept;

    /// @brief rate-limit을 통과한 실패 사유를 누적 건수와 함께 기록
    void logFailure(const std::string& message) const;

    /**
     * @brief   크기 정규화된 행렬에서 분모가 이 값 이하면 지평선에 너무 가깝다고 판단
     * @details 생성자가 max(|h_i|)=1 로 정규화하므로 H의 임의 스칼라배에 영향받지 않음
     */
    static constexpr double kMinDenominator = 1e-9;

    /// @brief max(|h_i|)=1 이고 화면 하단 중앙의 분모가 양수가 되도록 정규화된 행렬
    std::array<double, 9> matrix_;

    Options options_;

    /// @brief 변환 실패 로그 rate-limit 용 (파이프라인 스레드 단일 호출 전제)
    std::uint64_t failureCount_ = 0;
};