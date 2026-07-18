#include "transform/NullTransform.h"

std::optional<veda::WorldPoint> NullTransform::toWorld(const domain::ImagePoint& p) {
    veda::WorldPoint wp;
    wp.x = p.u;
    wp.y = p.v;
    return wp;
}