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

#include <memory>
#include <vector>

#include "Contract.h"
#include "interfaces/IChannelReceiver.h"
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
               std::shared_ptr<IHwEventDispatcher> dispatcher, std::shared_ptr<ISink> sink);

    ~Controller() = default;

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
};