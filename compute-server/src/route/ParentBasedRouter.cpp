#include "route/ParentBasedRouter.h"

#include <string>

#include "Contract.h"
#include "Logger.h"

namespace {
constexpr const char* kIface = "Router";
}  // namespace

RouteResult ParentBasedRouter::route(const domain::ChannelFrame& frame) {
    RouteResult result;
    // blur/risk 둘 다 최악의 경우 frame.objects 전체 크기만큼 커질 수 있으므로 미리 reserve
    // -> push_back 반복에 따른 다회 재할당을 1회로 줄임
    result.blur.reserve(frame.objects.size());
    result.risk.reserve(frame.objects.size());
    for (const auto& o : frame.objects) {
        // parentId 유무와 클래스(Head/LicensePlate) 둘 중 하나만 맞아도 blur로 보냄
        // -- Parent 속성 파싱 실패(parentId 없음)와 Type 문자열 미인식(cls=Unknown) 중
        //    어느 한쪽만 발생해도 나머지 신호로 구제되도록 함 (둘 다 동시에 실패해야 drop)
        if (o.parentId.has_value() || veda::isBlurClass(o.cls)) {
            result.blur.push_back(o);
        } else if (veda::isRiskClass(o.cls)) {
            result.risk.push_back(o);
        } else {
            // Unknown 클래스는 스트림에 섞여 들어오는 게 드물지 않아 프레임마다 반복될 수 있음
            // -- 정책상 정의된 drop 이므로 Debug (원인 추적은 Parser 의 Unknown Type 집계 로그가 담당)
            if (isLogEnabled(LogLevel::Debug)) {
                logDebug(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(o.id) +
                                     " cls=" + std::string(veda::toString(o.cls)) + " 분류 불가 - drop");
            }
        }
    }
    return result;
}