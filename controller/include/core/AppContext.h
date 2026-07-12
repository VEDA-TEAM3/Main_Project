/**
 * @file    AppContext.h
 * @brief   관제 서버의 모든 구현체를 조립하고 생명주기를 관리하는 최상위 컨테이너
 * @note    Controller는 인터페이스 포인터를 소유하지 않는다.
 *          AppContext가 모든 구현체를 생성/소유하고, Controller에는 non-owning 포인터만
 *          빌려준다. 이렇게 하면 "무엇을 쓸지 조립하는 책임"과 "그것으로 무엇을 할지
 *          실행하는 책임"이 분리된다.
 * @note    INetwork 구현체는 3개의 독립된 인스턴스로 생성한다: 수신 전용(Processor Server
 *          구독), IDriverSender 내부용(STM32 발행), IAppSender 내부용(App 발행). 세 인스턴스
 *          모두 동일한 mosquitto 브로커(라즈베리파이 로컬)에 연결되지만, 각자 다른 스레드에서
 *          사용되므로 인스턴스를 분리하여 동시성 문제를 원천적으로 피한다.
 * @todo    구현체는 .so로 개발되어 dlopen으로 로드된다는 전제이나, .so가 노출해야 하는
 *          팩토리 함수 계약이 아직 정의되지 않았다. 이 계약이 정해지기 전까지는
 *          unique_ptr의 기본 삭제자(delete)를 그대로 쓰지 않는다 — dlopen된 객체를
 *          기본 delete로 해제하는 것은 ABI 안전성 문제로 이어질 수 있다. 실제 구현 시
 *          커스텀 삭제자(팩토리의 destroy 함수를 호출하는 형태)로 교체해야 한다.
 */
#pragma once

#include <memory>

#include "core/AppConfig.h"
#include "core/Controller.h"
#include "interfaces/IAppSender.h"
#include "interfaces/IDriverSender.h"
#include "interfaces/INetwork.h"
#include "interfaces/IParser.h"
#include "interfaces/IRiskDetector.h"

/**
 * @brief   관제 서버 최상위 조립/생명주기 컨테이너
 */
class AppContext {
public:
    /**
     * @brief   AppConfig를 받아 모든 구현체를 조립하고 Controller를 생성한다.
     * @note    실제 조립 로직(dlopen, 구현체 생성, Controller 생성)은 .cpp에서 수행한다.
     * @param   config   관제 서버 설정값 (위험 거리 임계값, 프레임 주기 등)
     */
    explicit AppContext(const AppConfig& config);

    /**
     * @brief   소유 중인 모든 구현체와 Controller를 정리한다.
     * @note    Controller가 먼저 정지/소멸된 후, 구현체들이 정리되어야 한다
     *          (Controller가 구현체를 non-owning으로 참조하고 있으므로 순서가 중요하다).
     */
    ~AppContext();

    /**
     * @brief   Controller를 시작하여 파이프라인을 가동한다.
     */
    void run();

    /**
     * @brief   Controller를 정지시킨다.
     */
    void shutdown();

private:
    AppConfig config_; /**< 관제 서버 설정값 사본 */

    std::unique_ptr<INetwork> subscribeNetwork_; /**< Processor Server 수신 전용 */
    std::unique_ptr<INetwork> driverPubNetwork_; /**< IDriverSender 구현체 내부용 (STM32 발행) */
    std::unique_ptr<INetwork> appPubNetwork_;    /**< IAppSender 구현체 내부용 (App 발행) */

    std::unique_ptr<IParser> parser_;
    std::unique_ptr<IRiskDetector> riskDetector_;
    std::unique_ptr<IAppSender> appSender_;
    std::unique_ptr<IDriverSender> driverSender_;

    std::unique_ptr<Controller> controller_; /**< 위 구현체들을 non-owning으로 빌려 조립됨 */
};