#pragma once

#include <QColor>
#include <QString>

#include "model/DigitalTwinRuntimeConfig.h"
#include "model/DigitalTwinTypes.h"

struct DigitalTwinObjectVisualStyle {
    QString iconPath;
    /// 아이콘은 항상 정사각형이라 한 변만 있으면 된다
    int iconPixels = 0;
    QColor labelColor;
    QColor trailColor;
};

class DigitalTwinObjectStyleProvider {
public:
    virtual ~DigitalTwinObjectStyleProvider() = default;

    virtual DigitalTwinObjectVisualStyle styleFor(const DigitalTwinObject& object) const = 0;
};

class DefaultDigitalTwinObjectStyleProvider final : public DigitalTwinObjectStyleProvider {
public:
    explicit DefaultDigitalTwinObjectStyleProvider(DigitalTwinIconConfig iconConfig = {});

    DigitalTwinObjectVisualStyle styleFor(const DigitalTwinObject& object) const override;

private:
    DigitalTwinIconConfig iconConfig_;
};
