#include "transform/NullTransform.h"

veda::WorldPoint NullTransform::toWorld(const domain::ImagePoint& p) {
    veda::WorldPoint wp;
    wp.x = p.u;
    wp.y = p.v;
    return wp;
}