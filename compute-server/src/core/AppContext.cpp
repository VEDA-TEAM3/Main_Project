#include "core/AppContext.h"

#include "ground/BottomCenterExtractor.h"
#include "mapper/AffineImageCoordinateMapper.h"
#include "parser/OnvifParser.h"
#include "route/ParentBasedRouter.h"
#include "sanitize/ContainmentSanitizer.h"
#include "sink/ConsoleSink.h"
#include "source/RtspOnvifSource.h"
#include "transform/HomographyTransform.h"

AppContext::AppContext(const AppConfig& config) {
    // --- Source -------------------------------------------------------
    source_ = std::make_shared<RtspOnvifSource>(config);

    // --- Pipeline 구성 요소 --------------------------------------------
    auto parser = std::make_shared<OnvifParser>();
    auto imageMapper = std::make_shared<AffineImageCoordinateMapper>(config.imageMapScaleX, config.imageMapScaleY,
                                                                     config.imageMapOffsetX, config.imageMapOffsetY);
    auto sanitizer = std::make_shared<ContainmentSanitizer>(config.sanitizerIouThresh, config.sanitizerContainThresh);
    auto router = std::make_shared<ParentBasedRouter>();
    auto ground = std::make_shared<BottomCenterExtractor>();

    std::shared_ptr<ICoordinateTransform> transform;
    transform = std::make_shared<HomographyTransform>(config.homography);

    // --- Sink -----------------------------------------------------------
    auto riskSink = std::make_shared<ConsoleTopViewSink>();
    auto blurSink = std::make_shared<ConsoleBlurSink>();

    pipeline_ =
        std::make_unique<Pipeline>(parser, imageMapper, sanitizer, router, ground, transform, riskSink, blurSink);
}