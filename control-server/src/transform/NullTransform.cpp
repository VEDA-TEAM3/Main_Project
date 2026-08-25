#include "transform/NullTransform.h"

#include <utility>

void NullTransform::transform(const std::vector<veda::TopViewFrame>& in, std::vector<domain::ObservationFrame>& out) {
    out.clear();
    out.reserve(in.size());
    for (const auto& frame : in) {
        domain::ObservationFrame observed;
        observed.ts = frame.ts;
        observed.ch = frame.ch;
        observed.objects.reserve(frame.objects.size());
        for (const auto& obj : frame.objects)
            observed.objects.push_back(
                domain::WorldObservation{obj.id, obj.cls, domain::WorldPoint{obj.pos.x, obj.pos.y}});
        out.push_back(std::move(observed));
    }
}
