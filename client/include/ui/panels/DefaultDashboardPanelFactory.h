#pragma once

#include "ui/panels/DashboardPanelFactory.h"

class DefaultDashboardPanelFactory final : public DashboardPanelFactory {
public:
    DashboardPanels createPanels(const DashboardPanelHosts& hosts) const override;
};
