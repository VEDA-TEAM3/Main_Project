#pragma once

/**
 * @file     AppContext.h
 * @brief   애플리케이션의 모든 의존성을 조립하고 관리하는 DI 컨테이너
 */

#include <memory>

#include "core/AppConfig.h"
#include "core/Controller.h"

class AppContext {
public:
    /**
     * @brief 생성자에서 설정값을 주입
     * @param config 파이프라인 조립에 필요한 설정값
     */
    explicit AppContext(const AppConfig& config);
    ~AppContext() = default;

    /**
     * @brief 관제 서버의 핵심인 Controller와 모든 하위 부품을 조립하여 반환
     */
    std::shared_ptr<Controller> buildController();

private:
    AppConfig config_;
};