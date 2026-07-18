#pragma once

/**
 * @file    AppContext.h
 * @brief   config를 읽어 구현체를 조립하고 Pipeline을 완성하는 팩토리
 */

#include <memory>

#include "core/AppConfig.h"
#include "core/Pipeline.h"
#include "interfaces/IMetadataSource.h"

/**
 * @brief   구현체를 주입하는 팩토리
 */
class AppContext {
public:
    /**
     * @brief   config로 설정을 받아서 구현체를 주입
     * @param   config  연산 서버 설정
     */
    explicit AppContext(const AppConfig& config);

    /**
     * @brief   IMetadataSource의 구현체를 반환
     * @return  Source 구현체 참조자
     */
    IMetadataSource& source() { return *source_; }

    /**
     * @brief   Pipeline을 반환
     * @return  Pipeline 참조자
     */
    Pipeline& pipeline() { return *pipeline_; }

private:
    std::shared_ptr<IMetadataSource> source_;  ///< CCTV로부터 Metadata를 받아오는 Source 인스턴스
    std::unique_ptr<Pipeline> pipeline_;       ///< pipeline 인스턴스
};