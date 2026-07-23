#pragma once

/**
 * @file    Pipeline.h
 * @brief   연산 서버의 처리 순서를 정의하는 핵심 pipeline 클래스
 *
 * @details
 * 연산 서버의 pipeline은 수정되지 않음
 * -- 수정되는 부분은 각 단계의 실제 구현체 (AppContext에서 주입)
 *
 * @par [ 처리 흐름 ]
 * @code
 * Parser -> Sanitizer -> Router --+-- risk -> Ground -> Transform -> RiskSink
 *                                 +-- blur -> ImageMapper ---------> BlurSink
 * @endcode
 *
 * @note [ 의존성 주입 ]
 * 이 파일은 구현체 클래스를 전혀 모름
 * -- 정책 및 구현은 수정될 수 있으나 역할은 바뀌지 않음
 *
 * @note [ ImageMapper 가 Router 뒤 blur 분기에 있는 이유 ]
 * 예전에는 Parser 바로 뒤(분기 이전)에 있어서 risk 경로까지 앱 표시 좌표계로 변환됐음
 * 호모그래피는 Metadata 이미지 평면에서 캘리브레이션되므로, 이 매핑이 risk 경로에 섞이면
 * imageMapScale/Offset 을 blur 정합용으로 조정하는 순간 월드 좌표가 조용히 틀어짐
 * -- 기본값이 항등(scale=1, offset=0)이라 증상이 드러나지 않는 잠복 결함이었음
 * -- 자세한 배경은 IImageCoordinateMapper.h 참고
 *
 * @note [ risk 를 blur 보다 먼저 처리하는 이유 ]
 * risk 경로가 안전 크리티컬한 실시간 경로이므로 sink 로 나가는 시점을 앞당김 (비용 0)
 *
 * @note [ Network, Source 제외 ]
 * CCTV로부터 Metadata를 받아오는 단계는 pipeline에서 제외
 * -- 추가해도 무방하나 일단 제외하고 설계
 * -- main에서 처리
 */

#include <cstdint>
#include <memory>

#include "Contract.h"
#include "domain/RawPacket.h"
#include "interfaces/ICoordinateTransform.h"
#include "interfaces/IGroundPointExtractor.h"
#include "interfaces/IImageCoordinateMapper.h"
#include "interfaces/IMetadataParser.h"
#include "interfaces/IObjectRouter.h"
#include "interfaces/IObjectSanitizer.h"
#include "interfaces/ISink.h"

/**
 * @brief   bbox 가 잘린 risk 객체를 어떻게 처리할지 결정하는 정책
 * @details 문자열 ↔ 열거형 변환은 AppContext 가 담당 (AppConfig::riskEdgePolicy)
 */
enum class RiskEdgePolicy {
    Keep,                 ///< 그대로 통과 (edge 플래그만 실어 보냄)
    DropBottomTruncated,  ///< bbox 아래변이 잘린 객체만 폐기 (기본값)
    DropAnyEdge,          ///< 어느 변이든 경계에 닿으면 폐기
};

/**
 * @brief   pipeline 이 참조하는 실행 정책 모음
 * @details 정책이 늘어나도 Pipeline 생성자 시그니처가 흔들리지 않도록 구조체로 묶음
 */
struct PipelineOptions {
    RiskEdgePolicy edgePolicy = RiskEdgePolicy::DropBottomTruncated;
};

/**
 * @brief 연산 서버의 pipeline
 */
class Pipeline {
public:
    /**
     * @brief pipeline을 구성할 각 단계의 인터페이스 구현체들을 주입받아 초기화
     *
     * @param parser        Metadata 파싱
     * @param imageMapper   CCTV -> App 좌표 매핑 (blur 경로 전용)
     * @param sanitizer     팬텀 객체 제거
     * @param router        객체 분류 (Blur/Risk)
     * @param ground        지면 접촉점 추출
     * @param transform     카메라 로컬 지면 좌표 변환
     * @param riskSink      Risk 객체 전송
     * @param blurSink      Blur 객체 전송
     * @param options       실행 정책 (잘린 객체 처리 등)
     */
    Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IImageCoordinateMapper> imageMapper,
             std::shared_ptr<IObjectSanitizer> sanitizer, std::shared_ptr<IObjectRouter> router,
             std::shared_ptr<IGroundPointExtractor> ground, std::shared_ptr<ICoordinateTransform> transform,
             std::shared_ptr<ISink<veda::TopViewFrame>> riskSink, std::shared_ptr<ISink<veda::BlurFrame>> blurSink,
             const PipelineOptions& options = {});

    /**
     * @brief RawPacket 하나를 처리하여 pipeline에 추가
     *
     * @param raw 처리를 시작할 원본 Metadata 패킷
     *
     * @note [ 호출 Thread ]
     * Source를 돌리는 Thread에서 호출
     *
     * @note [ 논블로킹 보장 ]
     */
    void onPacket(const domain::RawPacket& raw);

private:
    std::shared_ptr<IMetadataParser> parser_;              ///< Metadata 파서 인스턴스
    std::shared_ptr<IImageCoordinateMapper> imageMapper_;  ///< Metadata 좌표계 → App 좌표계 매핑 인스턴스
    std::shared_ptr<IObjectSanitizer> sanitizer_;          ///< 팬텀 객체 제거 인스턴스
    std::shared_ptr<IObjectRouter> router_;                ///< 객체 분류 라우터 인스턴스
    std::shared_ptr<IGroundPointExtractor> ground_;        ///< 지면점 추출 인스턴스
    std::shared_ptr<ICoordinateTransform> transform_;      ///< 카메라 로컬 지면 좌표 변환 인스턴스
    std::shared_ptr<ISink<veda::TopViewFrame>> riskSink_;  ///< Risk 경로 결과물 출력 싱크 인스턴스
    std::shared_ptr<ISink<veda::BlurFrame>> blurSink_;     ///< Blur 경로 결과물 출력 싱크 인스턴스

    PipelineOptions options_;  ///< 실행 정책

    /// @name 폐기 사유별 누적 카운터 (로그 rate-limit 용, onPacket 은 단일 스레드 호출 전제)
    /// @{
    std::uint64_t edgeDropCount_ = 0;       ///< 잘림 정책으로 버린 risk 객체 수
    std::uint64_t transformFailCount_ = 0;  ///< 로컬 좌표 산출 실패로 버린 risk 객체 수
    /// @}
};