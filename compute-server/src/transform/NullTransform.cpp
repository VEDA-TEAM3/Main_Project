#include "transform/NullTransform.h"

std::optional<veda::LocalPoint> NullTransform::toLocal(const domain::ImagePoint& p) {
    veda::LocalPoint wp;
    wp.x = p.u;
    wp.y = p.v;
    return wp;
}