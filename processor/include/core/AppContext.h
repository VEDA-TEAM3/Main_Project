/**
 * @file    AppContext.h
 * @brief   각 인터페이스 구현체를 생성하고 관리하는 의존성 주입(DI) 컨테이너
 */
#pragma once

#include <memory>

#include "core/AppConfig.h"
#include "interfaces/INetwork.h"
#include "interfaces/IParser.h"
#include "interfaces/ISender.h"
#include "interfaces/ITrans.h"

/**
 * @brief   의존성 주입 클래스
 */
class AppContext {
public:
    /**
     * @brief   AppContext 생성자
     * @param   config  CCTV 연결 및 동작에 필요한 설정값
     */
    explicit AppContext(AppConfig config) : config_(std::move(config)) {}

    /**
     * @brief   Network, Parser, Trans, Sender 구현체를 생성하여 의존성을 주입
     * @return  모든 구현체가 정상적으로 생성되면 true, 하나라도 실패하면 false
     */
    bool initialize();

    /**
     * @name    구현체 접근자
     * @brief   initialize() 호출 이후 생성된 각 인터페이스 구현체를 반환
     * @{
     */
    std::shared_ptr<INetwork> network() const { return network_; }
    std::shared_ptr<IParser> parser() const { return parser_; }
    std::shared_ptr<ITrans> trans() const { return trans_; }
    std::shared_ptr<ISender> sender() const { return sender_; }
    /** @} */

private:
    AppConfig config_;                  /**< AppConfig 객체 */
    std::shared_ptr<INetwork> network_; /**< Network 구현체 */
    std::shared_ptr<IParser> parser_;   /**< Parser 구현체 */
    std::shared_ptr<ITrans> trans_;     /**< Trans 구현체 */
    std::shared_ptr<ISender> sender_;   /**< Sender 구현체 */
};