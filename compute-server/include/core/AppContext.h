#pragma once

/**
 * @file    AppContext.h
 * @brief   config를 읽어 구현체를 조립하고 Pipeline을 완성하는 팩토리
 */

#include <memory>

#include "core/AppConfig.h"
#include "core/Pipeline.h"
#include "interfaces/IMetadataSource.h"
#include "interfaces/IMqttTransport.h"

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

    /**
     * @brief   두 Sink가 공유하는 단일 MQTT 커넥션
     * @note    pipeline_ 보다 먼저 선언 -> 파괴는 역순이므로 Sink(워커 스레드가
     *          transport를 참조함)가 먼저 정리된 뒤에 transport가 사라짐
     *          (Sink도 shared_ptr로 잡고 있어 이중으로 안전하지만 선언 순서로도 명시)
     */
    std::shared_ptr<IMqttTransport> transport_;

    std::unique_ptr<Pipeline> pipeline_;  ///< pipeline 인스턴스
};