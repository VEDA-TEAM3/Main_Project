#include "ui/panels/DefaultDashboardPanelFactory.h"

#include <QLayout>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/panels/DeviceStatusPanel.h"
#include "ui/panels/EventLogPanel.h"
#include "ui/panels/ObjectListPanel.h"

namespace {
/**
 * @brief         패널 host에 여백 없는 layout이 없으면 생성합니다.
 * @param host    패널을 배치할 UI placeholder
 * @return        패널을 추가할 layout, host가 없으면 nullptr
 */
QLayout* ensureHostLayout(QWidget* host) {
    if (!host) {
        return nullptr;
    }

    if (host->layout()) {
        return host->layout();
    }

    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return layout;
}

/**
 * @brief         지정한 패널을 host의 Qt 소유권 아래 생성하고 배치합니다.
 * @tparam Panel  생성할 QWidget 기반 패널 형식
 * @param host    패널을 소유할 UI placeholder
 * @return        생성된 패널, host가 없으면 nullptr
 */
template <typename Panel>
Panel* createPanel(QWidget* host) {
    QLayout* layout = ensureHostLayout(host);

    if (!layout) {
        return nullptr;
    }

    auto* panel = new Panel(host);
    layout->addWidget(panel);
    return panel;
}
}  // namespace

/**
 * @brief        기본 대시보드 패널들을 각 placeholder에 생성합니다.
 * @param hosts  장비 상태, 이벤트 로그, 객체 목록 host 묶음
 * @return       생성된 패널 포인터 묶음
 */
DashboardPanels DefaultDashboardPanelFactory::createPanels(const DashboardPanelHosts& hosts) const {
    DashboardPanels panels;
    panels.deviceStatusPanel = createPanel<DeviceStatusPanel>(hosts.deviceStatusHost);
    panels.eventLogPanel = createPanel<EventLogPanel>(hosts.eventLogHost);
    panels.objectListPanel = createPanel<ObjectListPanel>(hosts.objectListHost);
    return panels;
}
