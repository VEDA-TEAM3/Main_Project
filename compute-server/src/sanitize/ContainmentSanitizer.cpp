
#include "sanitize/ContainmentSanitizer.h"

#include <algorithm>

#include "Contract.h"

namespace {

/**
 * @brief   bbox의 면적을 계산
 * @param   box 대상 bbox ([0,1] 정규화 좌표)
 * @return  면적 (폭 또는 높이가 음수면 0)
 */
double area(const domain::NormBox& box) {
    const double w = box.r - box.l;
    const double h = box.b - box.t;
    if (w <= 0.0 || h <= 0.0)
        return 0.0;
    return w * h;
}

/**
 * @brief   두 bbox의 교집합 면적을 계산
 * @param   a  bbox A
 * @param   b  bbox B
 * @return  교집합 면적 (겹치지 않으면 0)
 */
double intersectionArea(const domain::NormBox& a, const domain::NormBox& b) {
    const double l = std::max(a.l, b.l);
    const double t = std::max(a.t, b.t);
    const double r = std::min(a.r, b.r);
    const double bt = std::min(a.b, b.b);
    if (r <= l || bt <= t)
        return 0.0;
    return (r - l) * (bt - t);
}

/**
 * @brief   IoU(Intersection over Union) 를 계산
 * @param   a  bbox A
 * @param   b  bbox B
 * @return  IoU 값 [0,1] (합집합 면적이 0이면 0)
 */
double iou(const domain::NormBox& a, const domain::NormBox& b) {
    const double inter = intersectionArea(a, b);
    const double uni = area(a) + area(b) - inter;
    if (uni <= 0.0)
        return 0.0;
    return inter / uni;
}

/**
 * @brief   IoMin(교집합 / 두 면적 중 작은 쪽) 을 계산
 * @details
 * 포함 관계 판정에 사용
 * IoU와 달리 크기 차이가 큰 두 bbox에서도 작은 쪽이 큰 쪽 안에 거의 다 들어있는지를 정확히 반영
 * @param   a  bbox A
 * @param   b  bbox B
 * @return  IoMin 값 [0,1] (두 면적 중 작은 쪽이 0이면 0)
 */
double ioMin(const domain::NormBox& a, const domain::NormBox& b) {
    const double inter = intersectionArea(a, b);
    const double minArea = std::min(area(a), area(b));
    if (minArea <= 0.0)
        return 0.0;
    return inter / minArea;
}

}  // namespace

ContainmentSanitizer::ContainmentSanitizer(double iouThresh, double containThresh)
    : iouThresh_(iouThresh), containThresh_(containThresh) {}

domain::ChannelFrame ContainmentSanitizer::sanitize(domain::ChannelFrame frame) {
    const size_t n = frame.objects.size();
    std::vector<bool> drop(n, false);

    for (size_t i = 0; i < n; ++i) {
        const auto& x = frame.objects[i];
        if (!veda::isRiskClass(x.cls))
            continue;

        for (size_t j = 0; j < n; ++j) {
            if (i == j)
                continue;
            const auto& y = frame.objects[j];

            // 규칙 A
            if (veda::isBlurClass(y.cls) && iou(x.box, y.box) > iouThresh_) {
                drop[i] = true;
                break;
            }

            // 규칙 B
            if (x.cls == y.cls && area(x.box) < area(y.box) && ioMin(x.box, y.box) > containThresh_) {
                drop[i] = true;
                break;
            }
        }
    }

    domain::ChannelFrame result;
    result.utcTime = frame.utcTime;
    result.channelId = frame.channelId;
    result.objects.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!drop[i])
            result.objects.push_back(std::move(frame.objects[i]));
    }
    return result;
}