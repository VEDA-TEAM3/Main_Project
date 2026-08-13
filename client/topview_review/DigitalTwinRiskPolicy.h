#pragma once

#include "model/DigitalTwinTypes.h"

class DigitalTwinRiskPolicy {
public:
    virtual ~DigitalTwinRiskPolicy() = default;

    virtual DigitalTwinRiskLevel riskLevelForObjects(const DigitalTwinObject& firstObject,
                                                     const DigitalTwinObject& secondObject) const = 0;
};

class DistanceRiskPolicy final : public DigitalTwinRiskPolicy {
public:
    DigitalTwinRiskLevel riskLevelForObjects(const DigitalTwinObject& firstObject,
                                             const DigitalTwinObject& secondObject) const override;
};
