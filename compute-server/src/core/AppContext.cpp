#include "core/AppContext.h"

#include "ground/BottomCenterExtractor.h"
#include "mapper/AffineImageCoordinateMapper.h"
#include "parser/OnvifParser.h"
#include "route/ParentBasedRouter.h"
#include "sanitize/ContainmentSanitizer.h"
#include "sink/MqttBlurSink.h"
#include "sink/MqttTopViewSink.h"
#include "source/RtspOnvifSourceV2.h"
#include "transform/HomographyTransform.h"

AppContext::AppContext(const AppConfig& config) {
    source_ = std::make_shared<RtspOnvifSourceV2>(config);

    auto parser = std::make_shared<OnvifParser>();
    auto imageMapper = std::make_shared<AffineImageCoordinateMapper>(config.imageMapScaleX, config.imageMapScaleY,
                                                                     config.imageMapOffsetX, config.imageMapOffsetY);
    auto sanitizer = std::make_shared<ContainmentSanitizer>(config.sanitizerIouThresh, config.sanitizerContainThresh);
    auto router = std::make_shared<ParentBasedRouter>();
    auto ground = std::make_shared<BottomCenterExtractor>();
    auto transform = std::make_shared<HomographyTransform>(config.homography);

    auto riskSink = std::make_shared<MqttTopViewSink>(config);
    auto blurSink = std::make_shared<MqttBlurSink>(config);

    pipeline_ =
        std::make_unique<Pipeline>(parser, imageMapper, sanitizer, router, ground, transform, riskSink, blurSink);
}