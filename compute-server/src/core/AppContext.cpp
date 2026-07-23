#include "core/AppContext.h"

#include "ground/BottomCenterExtractor.h"
#include "mapper/AffineImageCoordinateMapper.h"
#include "mqtt/MqttTransport.h"
#include "parser/OnvifParser.h"
#include "route/ParentBasedRouter.h"
#include "sanitize/ContainmentSanitizer.h"
#include "sink/MqttBlurSink.h"
#include "sink/MqttTopViewSink.h"
#include "source/RtspOnvifSourceV2.h"
#include "transform/HomographyTransform.h"

namespace {

/// @brief AppConfig 의 문자열 정책을 pipeline 열거형으로 변환 (값 검증은 AppConfig::load 가 이미 수행)
RiskEdgePolicy toRiskEdgePolicy(const std::string& value) {
    if (value == "keep")
        return RiskEdgePolicy::Keep;
    if (value == "dropAnyEdge")
        return RiskEdgePolicy::DropAnyEdge;
    return RiskEdgePolicy::DropBottomTruncated;
}

HomographyTransform::Options toHomographyOptions(const AppConfig& config) {
    HomographyTransform::Options options;
    options.pixelSpace = config.homographySpace == "pixel";
    options.imageWidth = config.imageWidth;
    options.imageHeight = config.imageHeight;
    options.boundsEnabled = config.localBoundsEnabled;
    options.minX = config.localMinX;
    options.maxX = config.localMaxX;
    options.minY = config.localMinY;
    options.maxY = config.localMaxY;
    return options;
}

}  // namespace

AppContext::AppContext(const AppConfig& config) {
    source_ = std::make_shared<RtspOnvifSourceV2>(config);

    auto parser = std::make_shared<OnvifParser>(config.edgeEpsilon);
    auto imageMapper = std::make_shared<AffineImageCoordinateMapper>(config.imageMapScaleX, config.imageMapScaleY,
                                                                     config.imageMapOffsetX, config.imageMapOffsetY);
    auto sanitizer = std::make_shared<ContainmentSanitizer>(config.sanitizerIouThresh, config.sanitizerContainThresh);
    auto router = std::make_shared<ParentBasedRouter>();
    auto ground = std::make_shared<BottomCenterExtractor>();
    auto transform = std::make_shared<HomographyTransform>(config.homography, toHomographyOptions(config));

    // 두 sink가 공유하는 단일 MQTT 커넥션.
    // 순서가 중요함: transport 생성 -> sink 생성 및 start()(연결 리스너 등록) -> transport 시작
    // 그래야 최초 연결 이벤트를 어느 sink도 놓치지 않음
    transport_ = std::make_shared<MqttTransport>(config);

    auto riskSink = std::make_shared<MqttTopViewSink>(transport_, config);
    auto blurSink = std::make_shared<MqttBlurSink>(transport_, config);
    riskSink->start();
    blurSink->start();

    transport_->start();

    PipelineOptions options;
    options.edgePolicy = toRiskEdgePolicy(config.riskEdgePolicy);

    pipeline_ = std::make_unique<Pipeline>(parser, imageMapper, sanitizer, router, ground, transform, riskSink,
                                           blurSink, options);
}
