#pragma once

#include "domain/ChannelFrame.h"

class IImageCoordinateMapper {
public:
    virtual ~IImageCoordinateMapper() = default;
    virtual domain::ChannelFrame map(domain::ChannelFrame frame) const = 0;
};
