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
 * Parser -> ImageMapper -> Sanitizer -> Router --+-- blur ------------------------> BlurSink
 *                                                +-- risk -> Ground -> Transform -> RiskSink
 * @endcode
 *
 * @note [ 의존성 주입 ]
 * 이 파일은 구현체 클래스를 전혀 모름
 * -- 정책 및 구현은 수정될 수 있으나 역할은 바뀌지 않음
 *
 * @note [ Network, Source 제외 ]
 * CCTV로부터 Metadata를 받아오는 단계는 pipeline에서 제외
 * -- 추가해도 무방하나 일단 제외하고 설계
 * -- main에서 처리
 */

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
 * @brief 연산 서버의 pipeline
 */
class Pipeline {
public:
    /**
     * @brief pipeline을 구성할 각 단계의 인터페이스 구현체들을 주입받아 초기화
     *
     * @param parser        Metadata 파싱
     * @param imageMapper   CCTV -> App 좌표 매핑
     * @param sanitizer     팬텀 객체 제거
     * @param router        객체 분류 (Blur/Risk)
     * @param ground        지면 접촉점 추출
     * @param transform     월드 좌표 변환
     * @param riskSink      Risk 객체 전송
     * @param blurSink      Blur 객체 전송
     */
    Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IImageCoordinateMapper> imageMapper,
             std::shared_ptr<IObjectSanitizer> sanitizer, std::shared_ptr<IObjectRouter> router,
             std::shared_ptr<IGroundPointExtractor> ground, std::shared_ptr<ICoordinateTransform> transform,
             std::shared_ptr<ISink<veda::TopViewFrame>> riskSink, std::shared_ptr<ISink<veda::BlurFrame>> blurSink);

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
    std::shared_ptr<ICoordinateTransform> transform_;      ///< 월드 좌표 변환 인스턴스
    std::shared_ptr<ISink<veda::TopViewFrame>> riskSink_;  ///< Risk 경로 결과물 출력 싱크 인스턴스
    std::shared_ptr<ISink<veda::BlurFrame>> blurSink_;     ///< Blur 경로 결과물 출력 싱크 인스턴스
};