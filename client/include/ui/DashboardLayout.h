#pragma once

#include <QMainWindow>
#include <QSizePolicy>
#include <QWidget>

namespace DashboardLayout {
inline int recommendedBottomSectionHeight(int windowHeight) {
    constexpr int minimumBottomSectionHeight = 260;
    constexpr int maximumBottomSectionHeight = 320;
    constexpr int bottomSectionHeightRatioPercent = 30;

    const int scaledHeight = windowHeight * bottomSectionHeightRatioPercent / 100;

    if (scaledHeight < minimumBottomSectionHeight) {
        return minimumBottomSectionHeight;
    }

    if (scaledHeight > maximumBottomSectionHeight) {
        return maximumBottomSectionHeight;
    }

    return scaledHeight;
}

template <typename UiMainWindow>
inline void adjustBottomSectionHeight(QMainWindow* window, UiMainWindow* ui) {
    if (!window || !ui) {
        return;
    }

    ui->bottomSectionFrame->setFixedHeight(recommendedBottomSectionHeight(window->height()));
}

template <typename UiMainWindow>
inline void orderBottomSectionWidgets(UiMainWindow* ui) {
    if (!ui) {
        return;
    }

    ui->bottomSectionLayout->removeWidget(ui->objectListCard);
    ui->bottomSectionLayout->removeWidget(ui->eventLogCard);
    ui->bottomSectionLayout->removeWidget(ui->deviceStatusCard);

    ui->bottomSectionLayout->insertWidget(0, ui->objectListCard);
    ui->bottomSectionLayout->insertWidget(1, ui->eventLogCard);
    ui->bottomSectionLayout->insertWidget(2, ui->deviceStatusCard);
}

template <typename UiMainWindow>
inline void apply(QMainWindow* window, UiMainWindow* ui) {
    if (!window || !ui) {
        return;
    }

    window->setMinimumSize(1280, 720);

    ui->mainContentLayout->setContentsMargins(18, 0, 26, 14);
    ui->topBarLayout->setContentsMargins(18, 0, 8, 0);
    ui->topSectionLayout->setContentsMargins(0, 0, 0, 0);
    ui->bottomSectionLayout->setContentsMargins(0, 0, 0, 0);

    ui->topBarFrame->setFixedHeight(62);
    ui->legendFrame->setFixedHeight(46);

    ui->topSectionFrame->setMinimumHeight(0);
    ui->topSectionFrame->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->topSectionFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    adjustBottomSectionHeight(window, ui);
    ui->bottomSectionFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    ui->cctvCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->mapCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    ui->eventLogCard->setMinimumWidth(0);
    ui->deviceStatusCard->setMinimumWidth(0);
    ui->objectListCard->setMinimumWidth(0);
    ui->eventLogBodyFrame->setMinimumWidth(0);
    ui->deviceStatusBodyFrame->setMinimumWidth(0);
    ui->objectListBodyFrame->setMinimumWidth(0);

    ui->eventLogCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->deviceStatusCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->objectListCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    orderBottomSectionWidgets(ui);

    ui->mainContentLayout->setStretch(0, 0);
    ui->mainContentLayout->setStretch(1, 1);
    ui->mainContentLayout->setStretch(2, 0);
    ui->mainContentLayout->setStretch(3, 0);

    ui->topSectionLayout->setStretch(0, 9);
    ui->topSectionLayout->setStretch(1, 10);

    ui->bottomSectionLayout->setStretch(0, 7);
    ui->bottomSectionLayout->setStretch(1, 7);
    ui->bottomSectionLayout->setStretch(2, 11);

    ui->cctvCardLayout->setStretch(0, 0);
    ui->cctvCardLayout->setStretch(1, 1);
    ui->mapCardLayout->setStretch(0, 0);
    ui->mapCardLayout->setStretch(1, 1);

    ui->eventLogLayout->setStretch(0, 0);
    ui->eventLogLayout->setStretch(1, 1);
    ui->deviceStatusLayout->setStretch(0, 0);
    ui->deviceStatusLayout->setStretch(1, 1);
    ui->objectListLayout->setStretch(0, 0);
    ui->objectListLayout->setStretch(1, 1);

    ui->videoGridLayout->setRowStretch(0, 1);
    ui->videoGridLayout->setRowStretch(1, 1);
    ui->videoGridLayout->setColumnStretch(0, 1);
    ui->videoGridLayout->setColumnStretch(1, 1);
}
}  // namespace DashboardLayout
