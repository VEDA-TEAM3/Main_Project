#pragma once

/**
 * @file Pipeline.h
 * @brief 시스템의 데이터 처리 순서를 제어하는 핵심 파이프라인 클래스
 *
 * @details
 * 시스템의 처리 순서를 정의. 일반화하지 않음 -- 플러그인 체인 프레임워크가 아니라
 * 이 순서 자체가 코드로 고정된 것. 바뀌는 건 각 단계의 구현체이지 순서가 아님.
 *
 * @par [처리 흐름]
 * @code
 * Network -> Source -> Parser -> Sanitizer -> Router --+-- blur --------------------> BlurSink
 *                                                      +-- risk -> Ground -> Transform -> RiskSink
 * @endcode
 *
 * @note [의존성 주입]
 * 생성자는 인터페이스 포인터만 받음. 이 파일은 구체 클래스를 전혀 모름
 * 실제 조립은 AppContext 의 몫.
 */

#include <memory>

#include "Contract.h"
#include "domain/RawPacket.h"
#include "interfaces/ICoordinateTransform.h"
#include "interfaces/IGroundPointExtractor.h"
#include "interfaces/IMetadataParser.h"
#include "interfaces/IObjectRouter.h"
#include "interfaces/IObjectSanitizer.h"
#include "interfaces/ISink.h"

/**
 * @class Pipeline
 * @brief 수신된 원본 메타데이터를 파싱부터 최종 전송까지 일괄 처리하는 오케스트레이터
 */
class Pipeline {
public:
    /**
     * @brief 파이프라인을 구성할 각 단계의 인터페이스 구현체들을 주입받아 초기화.
     *
     * @param parser 메타데이터 파서
     * @param sanitizer 오탐지 객체 제거기
     * @param router 객체 분류기 (Blur/Risk)
     * @param ground 지면 접촉점 추출기
     * @param transform 월드 좌표 변환기
     * @param riskSink 위험 객체 전송 싱크
     * @param blurSink Blur 객체 전송 싱크
     */
    Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IObjectSanitizer> sanitizer,
             std::shared_ptr<IObjectRouter> router, std::shared_ptr<IGroundPointExtractor> ground,
             std::shared_ptr<ICoordinateTransform> transform, std::shared_ptr<ISink<veda::TopViewFrame>> riskSink,
             std::shared_ptr<ISink<veda::BlurFrame>> blurSink);

    /**
     * @brief IMetadataSource::next() 가 채운 RawPacket 하나를 처리하여 파이프라인에 흘려보냄.
     *
     * @param raw 처리를 시작할 원본 메타데이터 패킷
     *
     * @note [호출 스레드]
     * Source 를 돌리는 스레드에서 호출.
     *
     * @warning [논블로킹 보장]
     * 이 메서드는 내부적으로 블로킹하지 않음. 단, 주입된 sink 들이
     * ISink 규약(논블로킹)을 지켜야만 파이프라인 전체의 논블로킹이 보장.
     */
    void onPacket(const domain::RawPacket& raw);

private:
    std::shared_ptr<IMetadataParser> parser_;             ///< 메타데이터 파서 인스턴스
    std::shared_ptr<IObjectSanitizer> sanitizer_;         ///< 오탐지 객체 제거기 인스턴스
    std::shared_ptr<IObjectRouter> router_;               ///< 객체 분류 라우터 인스턴스
    std::shared_ptr<IGroundPointExtractor> ground_;       ///< 지면점 추출기 인스턴스
    std::shared_ptr<ICoordinateTransform> transform_;     ///< 월드 좌표 변환기 인스턴스
    std::shared_ptr<ISink<veda::TopViewFrame>> riskSink_;  ///< Risk 경로 결과물 출력 싱크
    std::shared_ptr<ISink<veda::BlurFrame>> blurSink_;     ///< Blur 경로 결과물 출력 싱크
};