#include "core/Pipeline.h"

#include <utility>

#include "domain/ChannelFrame.h"
#include "domain/DetectedObject.h"

Pipeline::Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IImageCoordinateMapper> imageMapper,
                   std::shared_ptr<IObjectSanitizer> sanitizer, std::shared_ptr<IObjectRouter> router,
                   std::shared_ptr<IGroundPointExtractor> ground, std::shared_ptr<ICoordinateTransform> transform,
                   std::shared_ptr<ISink<veda::TopViewFrame>> riskSink,
                   std::shared_ptr<ISink<veda::BlurFrame>> blurSink)
    : parser_(std::move(parser)),
      imageMapper_(std::move(imageMapper)),
      sanitizer_(std::move(sanitizer)),
      router_(std::move(router)),
      ground_(std::move(ground)),
      transform_(std::move(transform)),
      riskSink_(std::move(riskSink)),
      blurSink_(std::move(blurSink)) {}

namespace {

veda::BlurTarget toBlurTarget(const domain::DetectedObject& o) {
    veda::BlurTarget t;
    t.id = o.id;
    t.cls = o.cls;
    t.box = o.box;
    return t;
}

veda::TopViewObject toTopViewObject(const domain::DetectedObject& o, IGroundPointExtractor& ground,
                                    ICoordinateTransform& transform, bool& valid) {
    const domain::ImagePoint imgPoint = ground.extract(o.box);
    const auto worldPoint = transform.toWorld(imgPoint);
    valid = worldPoint.has_value();

    veda::TopViewObject out;
    out.id = o.id;
    out.cls = o.cls;
    out.pos = worldPoint.value_or(veda::WorldPoint{});
    out.edge = o.touchesBorder;
    return out;
}

}  // namespace

void Pipeline::onPacket(const domain::RawPacket& raw) {
    domain::ChannelFrame frame = parser_->parse(raw);
    frame = imageMapper_->map(std::move(frame));
    frame = sanitizer_->sanitize(std::move(frame));
    const RouteResult routed = router_->route(frame);

    veda::BlurFrame blurFrame;
    blurFrame.ts = frame.utcTime;
    blurFrame.ch = frame.channelId;
    blurFrame.blurs.reserve(routed.blur.size());
    for (const auto& o : routed.blur) {
        blurFrame.blurs.push_back(toBlurTarget(o));
    }
    blurSink_->send(blurFrame);

    veda::TopViewFrame riskFrame;
    riskFrame.ts = frame.utcTime;
    riskFrame.ch = frame.channelId;
    riskFrame.objects.reserve(routed.risk.size());
    for (const auto& o : routed.risk) {
        bool valid = true;
        auto tvo = toTopViewObject(o, *ground_, *transform_, valid);
        if (valid) {
            riskFrame.objects.push_back(std::move(tvo));
        } else {
            // TODO: 좌표 산출 불가 시 행동
        }
    }
    riskSink_->send(riskFrame);
}