/**
 * @file    Controller.h
 * @brief   관제 서버의 전체 파이프라인을 오케스트레이션하는 클래스
 * @note    실행 흐름은 두 개의 스레드로 구성된다.
 *          - 수신 스레드: INetwork::receive()로 들어오는 MetadataPacket을 즉시 처리한다.
 *            Human/Vehicle은 FrameAggregator에 반영하고, Face/LicensePlate는
 *            IAppSender::sendBlurTarget()으로 즉시 전송한다.
 *          - 타이머 스레드: frameIntervalMs 주기로 FrameAggregator::collectFrame()을 호출하여
 *            IRiskDetector::evaluate()를 실행하고, 결과를 IAppSender::sendTrackedFrame()과
 *            IDriverSender::sendChannelStatus()로 전송한다.
 * @note    인터페이스 포인터(INetwork, IParser, IRiskDetector, IAppSender, IDriverSender)는
 *          Controller가 소유하지 않는다. 생성/생명주기 관리는 AppContext의 책임이며,
 *          Controller는 이미 조립된 구현체를 빌려서 오케스트레이션만 담당한다.
 * @note    FrameAggregator는 인터페이스가 아닌 concrete 클래스이며 교체 대상이 아니므로,
 *          Controller가 값 멤버로 직접 소유한다.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "core/FrameAggregator.h"
#include "interfaces/IAppSender.h"
#include "interfaces/IDriverSender.h"
#include "interfaces/INetwork.h"
#include "interfaces/IParser.h"
#include "interfaces/IRiskDetector.h"

/**
 * @brief   관제 서버 파이프라인 오케스트레이터
 */
class Controller {
public:
    /**
     * @brief   Controller를 생성한다.
     * @note    전달되는 모든 인터페이스 포인터는 non-owning이다. 호출자(AppContext)가
     *          Controller보다 먼저 소멸시키지 않아야 한다.
     * @param   subscribeNetwork   Processor Server 수신 전용 INetwork 구현체
     * @param   parser             MetadataPacket 파싱 구현체
     * @param   riskDetector       위험 판단 구현체
     * @param   appSender          App 전송 구현체
     * @param   driverSender       STM32 전송 구현체
     * @param   frameIntervalMs    FrameAggregator 스냅샷 생성 주기 (ms)
     */
    Controller(INetwork* subscribeNetwork, IParser* parser, IRiskDetector* riskDetector, IAppSender* appSender,
               IDriverSender* driverSender, uint32_t frameIntervalMs);

    ~Controller();

    /**
     * @brief   수신 스레드와 타이머 스레드를 시작한다.
     */
    void start();

    /**
     * @brief   두 스레드를 정지시키고 join한다.
     */
    void stop();

private:
    /**
     * @brief   수신 스레드 루프. INetwork::receive()로 얻은 raw bytes를
     *          MetadataPacket으로 역직렬화한 뒤 IParser::parse()에 전달하고,
     *          결과에 따라 FrameAggregator 반영 또는 즉시 블러 전송을 수행한다.
     */
    void receiveLoop();

    /**
     * @brief   타이머 스레드 루프. frameIntervalMs 주기로 FrameAggregator::collectFrame(),
     *          IRiskDetector::evaluate()를 호출하고, 결과를 App/Driver로 전송한다.
     * @note    IDriverSender로의 전송 여부는 이 루프에서 판단한다
     *          (IDriverSender는 필터링 정책을 모른다).
     */
    void timerLoop();

    INetwork* subscribeNetwork_;  /**< non-owning */
    IParser* parser_;             /**< non-owning */
    IRiskDetector* riskDetector_; /**< non-owning */
    IAppSender* appSender_;       /**< non-owning */
    IDriverSender* driverSender_; /**< non-owning */

    FrameAggregator aggregator_; /**< Controller가 직접 소유 */

    uint32_t frameIntervalMs_; /**< 타이머 스레드 주기 (ms) */

    std::thread receiveThread_; /**< 수신 루프 실행 스레드 */
    std::thread timerThread_;   /**< 타이머 루프 실행 스레드 */
    std::atomic<bool> running_; /**< 두 스레드의 종료 조건 플래그 */
};