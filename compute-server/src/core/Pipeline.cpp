#include "core/Pipeline.h"

#include <utility>

#include "domain/ChannelFrame.h"
#include "domain/DetectedObject.h"

Pipeline::Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IObjectSanitizer> sanitizer,
                   std::shared_ptr<IObjectRouter> router, std::shared_ptr<IGroundPointExtractor> ground,
                   std::shared_ptr<ICoordinateTransform> transform, std::shared_ptr<ISink<veda::TopViewFrame>> riskSink,
                   std::shared_ptr<ISink<veda::BlurFrame>> blurSink)
    : parser_(std::move(parser)),
      sanitizer_(std::move(sanitizer)),
      router_(std::move(router)),
      ground_(std::move(ground)),
      transform_(std::move(transform)),
      riskSink_(std::move(riskSink)),
      blurSink_(std::move(blurSink)) {}

/* 추후 Builder 패턴 도입할 수도..? */
namespace {

veda::BlurTarget toBlurTarget(const domain::DetectedObject& o) {
    veda::BlurTarget t;
    t.id = o.id;
    t.cls = o.cls;
    t.box = o.box;
    return t;
}

veda::TopViewObject toTopViewObject(const domain::DetectedObject& o, IGroundPointExtractor& ground,
                                    ICoordinateTransform& transform) {
    const domain::ImagePoint imgPoint = ground.extract(o.box);
    const veda::WorldPoint worldPoint = transform.toWorld(imgPoint);

    veda::TopViewObject out;
    out.id = o.id;
    out.cls = o.cls;
    out.pos = worldPoint;
    out.conf = o.likelihood;
    out.edge = o.touchesBorder;
    return out;
}

}  // namespace

void Pipeline::onPacket(const domain::RawPacket& raw) {
    domain::ChannelFrame frame = parser_->parse(raw);
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
        riskFrame.objects.push_back(toTopViewObject(o, *ground_, *transform_));
    }
    riskSink_->send(riskFrame);
}