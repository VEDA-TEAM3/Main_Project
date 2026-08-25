#pragma once

class DeviceStatusPanel;
class EventLogPanel;
class ObjectListPanel;
class QWidget;

struct DashboardPanelHosts {
    QWidget* deviceStatusHost = nullptr;
    QWidget* eventLogHost = nullptr;
    QWidget* objectListHost = nullptr;
};

struct DashboardPanels {
    DeviceStatusPanel* deviceStatusPanel = nullptr;
    EventLogPanel* eventLogPanel = nullptr;
    ObjectListPanel* objectListPanel = nullptr;
};

class DashboardPanelFactory {
public:
    virtual ~DashboardPanelFactory() = default;

    virtual DashboardPanels createPanels(const DashboardPanelHosts& hosts) const = 0;
};
