#pragma once

#include <QColor>
#include <QSize>
#include <QString>

#include "model/DigitalTwinTypes.h"

struct DigitalTwinObjectVisualStyle {
    QString iconPath;
    QSize iconSize;
    QColor labelColor;
    QColor trailColor;
    QColor fallbackColor;
};

class DigitalTwinObjectStyleProvider {
public:
    virtual ~DigitalTwinObjectStyleProvider() = default;

    virtual DigitalTwinObjectVisualStyle styleFor(const DigitalTwinObject& object) const = 0;
};

class DefaultDigitalTwinObjectStyleProvider final : public DigitalTwinObjectStyleProvider {
public:
    DigitalTwinObjectVisualStyle styleFor(const DigitalTwinObject& object) const override;
};
