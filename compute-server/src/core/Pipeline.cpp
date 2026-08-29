#include "core/Pipeline.h"

#include <chrono>
#include <iomanip>
#include <sstream>
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
 * @brief   잘림 정책상 이 Risk 객체의 지면점을 신뢰할 수 없는지 판정
 * @details 아래변이 잘리면 발 위치를 모르는 채 잘린 지점을 지면으로 오인하므로
 *          호모그래피가 실제보다 훨씬 먼 곳으로 사상함
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
    const auto pipelineStartedAt = std::chrono::steady_clock::now();
    domain::ChannelFrame frame = parser_->parse(raw);
    frame = sanitizer_->sanitize(std::move(frame));
    // 멤버 버퍼를 재사용 → 프레임마다 RouteResult를 새로 만들지 않음 (힙 할당 0)
    router_->route(frame, routeResult_);
    RouteResult& routed = routeResult_;

    // ==== risk 경로 ====
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

    // ==== blur 경로 ====
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
    recordPipelineDuration(pipelineStartedAt, std::chrono::steady_clock::now());
}

void Pipeline::recordPipelineDuration(std::chrono::steady_clock::time_point startedAt,
                                      std::chrono::steady_clock::time_point completedAt) {
    totalPipelineDuration_ += completedAt - startedAt;
    ++pipelineSampleCount_;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(completedAt - metricsWindowStart_);
    if (elapsed < std::chrono::milliseconds(options_.metricsReportIntervalMs)) {
        return;
    }

    const double averageMs =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(totalPipelineDuration_).count() /
        static_cast<double>(pipelineSampleCount_);
    std::ostringstream report;
    report << std::fixed << std::setprecision(2) << "최근 " << elapsed.count() << "ms 지표 - compute 전체 파이프라인 "
           << pipelineSampleCount_ << "회, 평균 처리시간 " << averageMs << "ms";
    logSuccess(kIface, report.str());

    totalPipelineDuration_ = std::chrono::nanoseconds{0};
    pipelineSampleCount_ = 0;
    metricsWindowStart_ = completedAt;
}
