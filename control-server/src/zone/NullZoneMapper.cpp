#include "zone/NullZoneMapper.h"

void NullZoneMapper::assign(domain::WorldFrame& frame) {
    for (auto& obj : frame.objects) {
        obj.zoneId = 0;
    }
}