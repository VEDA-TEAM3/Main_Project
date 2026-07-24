#pragma once

/**
 * @file    Controller.h
 * @brief   관제 서버의 Pipeline
 *
 * @par [ 처리 흐름 ]
 * @code
 * ChannelReceiver → IFrameAggregator →
 * CrossChannelFuser → ZoneMapper → RiskPolicy
 * --+-- risk → HwEventDispatcher  → Sink
 *   +-- none ---------------------→ Sink
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Contract.h"
#include "domain/WorldObservation.h"
#include "interfaces/IChannelReceiver.h"
#include "interfaces/IClock.h"
#include "interfaces/ICrossChannelFuser.h"
#include "interfaces/IFrameAggregator.h"
#include "interfaces/IHwEventDispatcher.h"
#include "interfaces/ILocalToWorldTransform.h"
#include "interfaces/IRiskPolicy.h"
#include "interfaces/ISink.h"
#include "interfaces/IZoneMapper.h"

class Controller {
public:
    /**
     * @brief 의존성 주입(DI)을 통한 Controller생성
     */
    Controller(std::shared_ptr<IChannelReceiver> receiver, std::shared_ptr<IFrameAggregator> aggregator,
               std::shared_ptr<ILocalToWorldTransform> transform, std::shared_ptr<ICrossChannelFuser> fuser,
               std::shared_ptr<IZoneMapper> zoneMapper, std::shared_ptr<IRiskPolicy> riskPolicy,
               std::shared_ptr<IHwEventDispatcher> dispatcher, std::shared_ptr<ISink> sink,
               std::shared_ptr<IClock> clock, int channelCount);

    /**
     * @brief   receiver를 먼저 stop()시켜 워커 스레드를 join한 뒤 소멸
     * @details 멤버는 선언 역순으로 소멸되어 aggregator_ 등이 receiver_보다 먼저 파괴되므로,
     *          receiver_의 워커가 살아있는 채로 콜백을 통해 이미 파괴된 멤버에 접근하는 것을
     *          방지하기 위해 명시적으로 정의함 (stop()은 멱등이라 이미 호출됐어도 안전)
     */
    ~Controller();

    /**
     * @brief 관제 서버 Pipeline 시작
     */
    void start();

    /**
     * @brief 관제 서버 Pipeline 중지
     */
    void stop();

private:
    /**
     * @brief 핵심 알고리즘 흐름을 정의한 템플릿 메서드
     * @param frames 시간 윈도우 내에 수집된 프레임 묶음
     */
    void processPipeline(std::vector<veda::TopViewFrame> frames);

    std::shared_ptr<IChannelReceiver> receiver_;
    std::shared_ptr<IFrameAggregator> aggregator_;
    std::shared_ptr<ILocalToWorldTransform> transform_;
    std::shared_ptr<ICrossChannelFuser> fuser_;
    std::shared_ptr<IZoneMapper> zoneMapper_;
    std::shared_ptr<IRiskPolicy> riskPolicy_;
    std::shared_ptr<IHwEventDispatcher> dispatcher_;
    std::shared_ptr<ISink> sink_;
    std::shared_ptr<IClock> clock_;

    /// @brief transform() 결과를 담는 재사용 버퍼 (윈도우마다 재할당하지 않기 위해 멤버로 둠).
    ///        processPipeline 은 단일 스레드에서만 도므로 락이 필요 없음
    std::vector<domain::ObservationFrame> observations_;

    /**
     * @name 채널 생존 상태 (LWT + STM32 하트비트)
     *
     * @details
     * 예전에는 MqttChannelReceiver 가 veda/ch/+/alive 를 구독까지 해놓고도
     * Controller 가 setAliveCallback() 을 부르지 않아 신호가 통째로 버려졌음
     * -- compute-server 가 LWT 를 발행해도 받는 쪽이 없어 '빈 프레임'과 '채널 사망'을
     *    끝내 구분하지 못하는 상태였음
     *
     * 이제 상태를 들고 전환(alive<->dead)마다 로그를 남기고, veda::ChannelStatus 로
     * 묶어 sink_->sendChannelStatus() 로 Qt 클라이언트에도 발행함 (ISink/Contract.h 참고)
     * -- cameraAlive(MQTT LWT)와 hardwareAlive(STM32 하트비트)를 하나의 메시지에 함께 실어
     *    보내므로, 셋 중 하나만 바뀌어도 최신 조합 전체를 다시 보냄
     * -- IHwEventDispatcher::StatusCallback이 hardwareAlive와 함께 실제 경광등/부저/LED
     *    표시 상태(HwIndicatorState)도 실어 보내므로, 그것도 같은 스냅샷에 포함해 보냄
     *
     * @note MQTT 콜백 스레드와 UART 리더 스레드 양쪽에서 갱신되므로 뮤텍스로 보호
     * @{
     */
    void onChannelAlive(veda::ChannelId channel, bool alive, const char* source);
    void onHardwareStatus(veda::ChannelId channel, bool alive, const HwIndicatorState& indicators);

    /**
     * @brief   현재 채널 상태(cameraAlive/hardwareAlive + 표시 상태)를 하나의 스냅샷으로 조립
     * @param   idx 채널 인덱스 (= channelId)
     * @return  발행 직전의 ChannelStatus (ts 는 호출자가 락 밖에서 채움)
     * @warning 호출자가 aliveMutex_ 를 잡은 상태에서만 호출할 것 (뮤텍스로 보호되는 멤버를 읽음)
     */
    veda::ChannelStatus buildStatusLocked(std::size_t idx) const;

    int channelCount_;
    std::mutex aliveMutex_;
    std::vector<bool> channelAlive_;                 ///< 인덱스 = channelId, MQTT LWT 기준
    std::vector<bool> hardwareAlive_;                ///< 인덱스 = channelId, STM32 하트비트 기준
    std::vector<HwIndicatorState> indicatorState_;    ///< 인덱스 = channelId, STM32가 보고한 마지막 표시 상태
    /** @} */
};