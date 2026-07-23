#include "core/Pipeline.h"

#include <string>
#include <utility>

#include "Logger.h"
#include "domain/ChannelFrame.h"
#include "domain/DetectedObject.h"

namespace {

constexpr const char* kIface = "Pipeline";

veda::BlurTarget toBlurTarget(const domain::DetectedObject& o) {
    veda::BlurTarget t;
    t.id = o.id;
    t.cls = o.cls;
    t.box = o.box;
    return t;
}

/**
 * @brief   잘림 정책상 이 risk 객체의 지면점을 신뢰할 수 없는지 판정
 * @details 아래변이 잘리면 발 위치를 모르는 채 잘린 지점을 지면으로 오인하므로
 *          호모그래피가 실제보다 훨씬 먼 곳으로 사상함 (IGroundPointExtractor.h 의 '잘림 현상')
 */
bool isEdgeRejected(const domain::DetectedObject& o, RiskEdgePolicy policy) {
    switch (policy) {
        case RiskEdgePolicy::DropBottomTruncated:
            return o.bottomTruncated;
        case RiskEdgePolicy::DropAnyEdge:
            return o.touchesBorder;
        case RiskEdgePolicy::Keep:
            break;
    }
    return false;
}

}  // namespace

Pipeline::Pipeline(std::shared_ptr<IMetadataParser> parser, std::shared_ptr<IImageCoordinateMapper> imageMapper,
                   std::shared_ptr<IObjectSanitizer> sanitizer, std::shared_ptr<IObjectRouter> router,
                   std::shared_ptr<IGroundPointExtractor> ground, std::shared_ptr<ICoordinateTransform> transform,
                   std::shared_ptr<ISink<veda::TopViewFrame>> riskSink,
                   std::shared_ptr<ISink<veda::BlurFrame>> blurSink, const PipelineOptions& options)
    : parser_(std::move(parser)),
      imageMapper_(std::move(imageMapper)),
      sanitizer_(std::move(sanitizer)),
      router_(std::move(router)),
      ground_(std::move(ground)),
      transform_(std::move(transform)),
      riskSink_(std::move(riskSink)),
      blurSink_(std::move(blurSink)),
      options_(options) {}

void Pipeline::onPacket(const domain::RawPacket& raw) {
    domain::ChannelFrame frame = parser_->parse(raw);
    frame = sanitizer_->sanitize(std::move(frame));
    RouteResult routed = router_->route(frame);

    // --- risk 경로 (안전 크리티컬: 먼저 내보냄) ---------------------------------
    // 좌표계는 파서가 만든 Metadata 이미지 평면 그대로임 (ImageMapper 를 거치지 않음)
    // -> 호모그래피가 캘리브레이션된 좌표계와 일치
    // 결과는 '카메라 로컬' 좌표 (도면 좌표로 옮기는 건 control-server 의 몫)
    veda::TopViewFrame riskFrame;
    riskFrame.ts = frame.utcTime;
    riskFrame.ch = frame.channelId;
    riskFrame.objects.reserve(routed.risk.size());

    for (const auto& o : routed.risk) {
        if (isEdgeRejected(o, options_.edgePolicy)) {
            ++edgeDropCount_;
            if (edgeDropCount_ == 1 || edgeDropCount_ % 100 == 0) {
                logError(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(o.id) +
                                     " bbox 잘림으로 지면점 신뢰 불가 - 폐기 (누적 " + std::to_string(edgeDropCount_) +
                                     "건)");
            }
            continue;
        }

        const domain::ImagePoint groundPoint = ground_->extract(o.box);
        const auto worldPoint = transform_->toLocal(groundPoint);
        if (!worldPoint.has_value()) {
            // 지평선 위/너머이거나 월드 범위를 벗어난 지면점
            // -- 사유별 상세 로그는 ICoordinateTransform 구현체가 rate-limit 해서 남기므로
            //    여기서는 '어느 객체가 몇 건 사라졌는지'만 집계
            ++transformFailCount_;
            if (transformFailCount_ == 1 || transformFailCount_ % 100 == 0) {
                logError(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(o.id) +
                                     " 로컬 좌표 산출 실패 - 폐기 (누적 " + std::to_string(transformFailCount_) +
                                     "건)");
            }
            continue;
        }

        veda::TopViewObject out;
        out.id = o.id;
        out.cls = o.cls;
        out.pos = *worldPoint;
        out.edge = o.touchesBorder;
        riskFrame.objects.push_back(out);
    }
    riskSink_->send(riskFrame);

    // --- blur 경로 -------------------------------------------------------------
    // 앱이 영상 위에 사각형을 얹어야 하므로 여기서만 앱 표시 좌표계로 매핑
    imageMapper_->map(routed.blur, frame.channelId);

    veda::BlurFrame blurFrame;
    blurFrame.ts = frame.utcTime;
    blurFrame.ch = frame.channelId;
    blurFrame.blurs.reserve(routed.blur.size());
    for (const auto& o : routed.blur) {
        blurFrame.blurs.push_back(toBlurTarget(o));
    }
    blurSink_->send(blurFrame);
}
