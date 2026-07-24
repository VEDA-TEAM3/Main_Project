
#include "sanitize/ContainmentSanitizer.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <string>
#include <utility>

#include "Contract.h"
#include "Logger.h"

namespace {

constexpr const char* kIface = "Sanitizer";

/// @brief 한 프레임(단일 카메라)의 최대 객체 수 상한. drop 마스크를 스택 std::bitset 으로 두어
///        hot path 에서 std::vector<bool> 힙 할당을 없애기 위한 컴파일타임 크기.
///        compute-server 는 채널당 1개 프로세스이고, 엣지 AI(YOLO 등)의 NMS 출력은 보통
///        프레임당 50~100개로 제한되므로 128 이면 충분한 여유가 있다. 넘으면 sanitize 스킵(fail-open).
constexpr std::size_t kMaxObjectsPerFrame = 128;

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

    // drop 마스크를 스택 bitset 으로 -> 프레임마다 std::vector<bool> 를 새로 할당하던 것을 제거
    // (Principle #3: hot path 힙 할당 0). 극히 드물게 객체가 상한을 넘으면(비정상 폭주) sanitize 를
    // 스킵한다: fail-open 이라 팬텀 제거만 못 할 뿐, 위험 객체를 지우지 않는 쪽이 안전 측면에서 낫다.
    if (n > kMaxObjectsPerFrame) {
        logError(kIface, "ch=" + std::to_string(frame.channelId) + " 객체 수 " + std::to_string(n) + " > " +
                             std::to_string(kMaxObjectsPerFrame) + " - sanitize 스킵");
        return frame;
    }
    std::bitset<kMaxObjectsPerFrame> drop;

    // 1단계: 판정만 수행 (i의 판정이 다른 모든 j의 "원본" 데이터를 참조하므로,
    // 이 단계가 끝나기 전에는 frame.objects를 절대 변형하면 안 됨)
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
                // 프레임마다 도는 정상 동작이라 Debug (문자열 조립도 레벨로 걸러냄)
                if (isLogEnabled(LogLevel::Debug)) {
                    logDebug(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(x.id) +
                                         " 제거 (규칙 A: " + std::string(veda::toString(y.cls)) +
                                         " id=" + std::to_string(y.id) + "와 IoU>" + std::to_string(iouThresh_) + ")");
                }
                break;
            }

            // 규칙 B
            if (x.cls == y.cls && area(x.box) < area(y.box) && ioMin(x.box, y.box) > containThresh_) {
                drop[i] = true;
                if (isLogEnabled(LogLevel::Debug)) {
                    logDebug(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(x.id) +
                                         " 제거 (규칙 B: id=" + std::to_string(y.id) + " 안에 포함)");
                }
                break;
            }
        }
    }

    // 2단계: 판정이 모두 끝난 뒤에만 frame.objects를 in-place로 압축
    // (별도 벡터를 새로 만들지 않음 -> resize()로 줄이는 건 재할당을 유발하지 않으므로 할당 없음)
    size_t writeIdx = 0;
    for (size_t i = 0; i < n; ++i) {
        if (drop[i])
            continue;
        if (writeIdx != i)
            frame.objects[writeIdx] = std::move(frame.objects[i]);
        ++writeIdx;
    }
    frame.objects.resize(writeIdx);
    return frame;
}