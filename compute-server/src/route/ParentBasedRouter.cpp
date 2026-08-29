#include <algorithm>
#include <string>

#include "route/ParentBasedRouter.h"

#include "Contract.h"
#include "Logger.h"

namespace {
constexpr const char* kIface = "Router";
}  // namespace

void ParentBasedRouter::route(const domain::ChannelFrame& frame, RouteResult& outResult) {
    // 호출자가 소유한 버퍼를 재사용한다. → clear()는 capacity를 유지하므로 재할당이 없다.
    // (예전에는 지역 RouteResult를 새로 만들어 값 반환했고, 그래서 프레임마다 힙 할당 2회가 발생했음)
    outResult.blur.clear();
    outResult.risk.clear();

    // blur/risk 둘 다 최악의 경우 frame.objects 전체 크기만큼 커질 수 있으므로 미리 reserve
    // → 첫 프레임에서만 실제 할당이 일어나고, 이후에는 capacity가 충분해 no-op이 됨
    outResult.blur.reserve(frame.objects.size());
    outResult.risk.reserve(frame.objects.size());

    for (const auto& o : frame.objects) {
        // 같은 parentId의 Head가 있으면 Face 대신 더 넓은 Head만 보낸다.
        // parentId가 없거나 다르면 동일인을 확정할 수 없으므로 Face를 유지한다.
        // ponytail: O(N²), 파서 상한 256을 늘릴 때 재사용 가능한 lookup으로 교체한다.
        const bool hasHeadForSameParent =
            o.isFace && o.parentId.has_value() &&
            std::any_of(frame.objects.begin(), frame.objects.end(), [&](const auto& other) {
                return !other.isFace && other.cls == veda::ObjectClass::Head && other.parentId == o.parentId;
            });
        if (hasHeadForSameParent) {
            continue;
        }

        // parentId 유무와 클래스(Head/LicensePlate) 둘 중 하나만 맞아도 blur로 보냄
        // -- Parent 속성 파싱 실패(parentId 없음)와 Type 문자열 미인식(cls=Unknown) 중
        //    어느 한쪽만 발생해도 나머지 신호로 구제되도록 함 (둘 다 동시에 실패해야 drop)
        if (o.parentId.has_value() || veda::isBlurClass(o.cls)) {
            outResult.blur.push_back(o);
        } else if (veda::isRiskClass(o.cls)) {
            outResult.risk.push_back(o);
        } else {
            // Unknown 클래스는 스트림에 섞여 들어오는 게 드물지 않아 프레임마다 반복될 수 있음
            // -- 정책상 정의된 drop이므로 Debug
            if (isLogEnabled(LogLevel::Debug)) {
                logDebug(kIface, "ch=" + std::to_string(frame.channelId) + " id=" + std::to_string(o.id) +
                                     " cls=" + std::string(veda::toString(o.cls)) + " 분류 불가 - drop");
            }
        }
    }
}