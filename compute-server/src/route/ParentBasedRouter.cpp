#include "route/ParentBasedRouter.h"

#include "Contract.h"

RouteResult ParentBasedRouter::route(const domain::ChannelFrame& frame) {
    RouteResult result;
    for (const auto& o : frame.objects) {
        if (o.parentId.has_value()) {
            result.blur.push_back(o);
        } else if (veda::isRiskClass(o.cls)) {
            result.risk.push_back(o);
        }
    }
    return result;
}