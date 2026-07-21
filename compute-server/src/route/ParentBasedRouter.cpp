#include "route/ParentBasedRouter.h"

#include <string>

#include "Contract.h"
#include "log/Logger.h"

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
        if (o.parentId.has_value()) {
            result.blur.push_back(o);
        } else if (veda::isRiskClass(o.cls)) {
            result.risk.push_back(o);
        } else {
            logError(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(o.id) + " cls=" +
                                  std::string(veda::toString(o.cls)) + " 분류 불가 - drop");
        }
    }
    return result;
}