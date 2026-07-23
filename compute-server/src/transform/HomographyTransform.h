#pragma once

/**
 * @file    HomographyTransform.h
 * @brief   정규화 이미지 좌표 → 월드 좌표 (Homography)
 */

#include <array>
#include <cstdint>
#include <string>

#include "interfaces/ICoordinateTransform.h"

/**
 * @brief   평면 호모그래피로 지면점을 카메라 로컬 지면 좌표(m)로 사상
 *
 * @details
 * toLocal() 가 받는 점은 언제나 [0,1] 정규화 좌표(좌상단 원점)임
 * 반면 캘리브레이션 도구(OpenCV findHomography 등)가 뱉는 행렬은 보통 픽셀 좌표계 기준이라
 * 그대로 넣으면 h6/h7 분모 항의 스케일이 어긋나 예외 없이 완전히 틀린 좌표가 나옴
 * -> Options::pixelSpace 로 입력 좌표계를 명시하면 생성자가 한 번만 환산해둠
 *    (H_norm = H_pixel * diag(W, H, 1) -> 열 0에 W, 열 1에 H 를 곱하는 것과 동일)
 *    런타임 경로에는 분기도 곱셈도 추가되지 않음
 *
 * @note [ 지평선 처리 ]
 * 분모 = h6*u + h7*v + h8 이 0 이 되는 직선이 곧 지평선임
 * 지평선 '너머'의 점은 분모 부호가 뒤집혀, 크기는 멀쩡하지만 반대편으로 반사된
 * 그럴듯한 좌표를 내놓음 -> 팬텀 객체가 엉뚱한 위치에 발행됨
 * 호모그래피는 스칼라배 불변이므로, 생성자에서 '화면 하단 중앙(0.5, 1.0)의 분모가
 * 양수'가 되도록 전체 부호를 정규화해두면 이후엔 분모 > 0 검사만으로 지평선 너머를 걸러낼 수 있음
 * (하단 중앙을 기준점으로 삼는 이유: 아래로 기울어진 CCTV 라면 화면 맨 아래 지면은
 *  반드시 카메라 앞쪽이므로. 좌상단(0,0)은 지평선 위일 수 있어 기준으로 부적합)
 */
class HomographyTransform final : public ICoordinateTransform {
public:
    /**
     * @brief 행렬 해석 방식과 사후 검증 범위를 담는 설정
     */
    struct Options {
        /// @brief true 면 matrix 를 픽셀 좌표계 기준으로 보고 생성자에서 정규화 좌표계로 환산
        bool pixelSpace = false;

        /// @brief pixelSpace 일 때 캘리브레이션에 사용한 이미지 해상도
        double imageWidth = 0.0;
        double imageHeight = 0.0;

        /// @brief 월드 좌표 유효 범위 검사 (주차장 도면 밖으로 발산한 값을 버림)
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
     * @throws  std::invalid_argument 유한하지 않은 값, pixelSpace 인데 해상도가 0 이하,
     *                                또는 행렬이 특이(가역이 아님)한 경우
     */
    HomographyTransform(std::array<double, 9> matrix, const Options& options);

    /// @brief 기본 설정(정규화 좌표계, 범위 검사 없음)
    explicit HomographyTransform(std::array<double, 9> matrix) : HomographyTransform(matrix, Options{}) {}

    std::optional<veda::LocalPoint> toLocal(const domain::ImagePoint& p) override;

private:
    /// @brief 변환 실패 사유를 rate-limit 해서 남김 (지평선 근처 객체는 매 프레임 실패하므로)
    void logFailure(const std::string& message);

    /// @brief 분모가 이 값보다 작으면 지평선에 너무 가까워 좌표가 발산한다고 판단
    static constexpr double kMinDenominator = 1e-9;

    std::array<double, 9> matrix_;

    /// @brief 부호 정규화에 성공했는가 (실패 시 분모 부호 검사를 생략하고 크기만 검사)
    bool horizonCheckEnabled_ = true;

    Options options_;

    /// @brief 변환 실패 로그 rate-limit 용 (파이프라인 스레드 단일 호출 전제)
    std::uint64_t failureCount_ = 0;
};
