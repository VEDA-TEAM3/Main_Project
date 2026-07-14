#pragma once

/**
 * @file    AppContext.h
 * @brief   config 를 읽어 구체 구현체를 조립하고 Pipeline 을 완성하는 팩토리
 */

#include <memory>

#include "core/AppConfig.h"
#include "core/Pipeline.h"
#include "interfaces/IMetadataSource.h"

/**
 * @class   AppContext
 * @brief   구현체가 실제로 갈아끼워지는 유일한 장소.
 */
class AppContext {
public:
    /**
     * @brief   AppContext 생성자. config 로 모든 구현체를 조립
     * @param   config  연산 서버 설정
     */
    explicit AppContext(const AppConfig& config);

    /**
     * @brief   조립이 끝난 IMetadataSource 를 반환
     * @return  Source 참조
     */
    IMetadataSource& source() { return *source_; }

    /**
     * @brief   조립이 끝난 Pipeline 을 반환
     * @return  Pipeline 참조
     */
    Pipeline& pipeline() { return *pipeline_; }

private:
    std::shared_ptr<IMetadataSource> source_;
    std::unique_ptr<Pipeline> pipeline_;
};