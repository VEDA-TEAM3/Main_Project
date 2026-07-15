#include "core/Controller.h"

Controller::Controller(std::shared_ptr<IChannelReceiver> receiver, std::shared_ptr<IFrameAggregator> aggregator,
                       std::shared_ptr<ICrossChannelFuser> fuser, std::shared_ptr<IZoneMapper> zoneMapper,
                       std::shared_ptr<IRiskPolicy> riskPolicy, std::shared_ptr<IHwEventDispatcher> dispatcher,
                       std::shared_ptr<ISink> sink)
    : receiver_(std::move(receiver)),
      aggregator_(std::move(aggregator)),
      fuser_(std::move(fuser)),
      zoneMapper_(std::move(zoneMapper)),
      riskPolicy_(std::move(riskPolicy)),
      dispatcher_(std::move(dispatcher)),
      sink_(std::move(sink)) {
    // 1. Receiver가 외부에서 프레임을 수신하면, 즉시 Aggregator로 푸시(Push)
    receiver_->setCallback([this](const veda::TopViewFrame& frame) { aggregator_->push(frame); });

    // 2. Aggregator가 시간 윈도우 단위로 집계를 완료하면, 파이프라인 실행
    aggregator_->setCallback([this](const std::vector<veda::TopViewFrame>& frames) { this->processPipeline(frames); });
}

void Controller::start() { receiver_->start(); }

void Controller::stop() { receiver_->stop(); }

void Controller::processPipeline(const std::vector<veda::TopViewFrame>& frames) {
    if (frames.empty()) {
        return;
    }

    // Step 1: 다중 채널 프레임 융합 (ConcatFuser: dedup 포함, gid 부여)
    auto worldFrame = fuser_->fuse(frames);

    // Step 2: 물리 좌표 → 액추에이터 채널(zone) 배정
    //         IRiskPolicy::evaluate() 의 @pre 조건 (zoneId 사전 배정)을 만족시키기 위해
    //         반드시 evaluate() 호출보다 먼저 실행되어야 함
    zoneMapper_->assign(worldFrame);

    // Step 3: 위험도 평가 (zoneId 기준 grouping된 zoneLevels 포함하여 반환)
    auto riskEval = riskPolicy_->evaluate(worldFrame);

    // Step 4: STM32로 위험 이벤트 통지 (HW 제어 자체는 STM32/FreeRTOS 담당,
    //         관제 서버는 LED/siren/buzzer 상태를 직접 결정하지 않음)
    dispatcher_->dispatch(riskEval);

    // Step 5: 클라이언트 대시보드로 전송 (HW와 동일한 riskEval 로직을 이미 반영한 worldFrame)
    sink_->send(worldFrame);
}