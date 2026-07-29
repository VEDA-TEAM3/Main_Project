cmake_minimum_required(VERSION 3.16)

project(Qtcctvclient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ProcessorCount)

find_package(Qt6 REQUIRED COMPONENTS Mqtt Network Widgets)

set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/mingw_x86_64" CACHE PATH
    "GStreamer MinGW x86_64 runtime root")

option(QTCCTV_ENABLE_PCH "Enable precompiled headers for faster Qt rebuilds" ON)
option(QTCCTV_ENABLE_UNITY_BUILD "Enable unity build for faster full rebuilds" OFF)

qt_standard_project_setup()

qt_add_executable(Qtcctvclient
    .clang-format
    .clang-tidy
    C_CppCodingConvention.md

    src/main.cpp

    include/config/ApplicationConfig.h
    src/config/ApplicationConfig.cpp

    include/ui/mainwindow.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.ui
    include/ui/ClickableVideoWidget.h
    src/ui/ClickableVideoWidget.cpp
    include/ui/dialogs/MapSettingsDialog.h
    src/ui/dialogs/MapSettingsDialog.cpp
    include/ui/dialogs/ReportConfirmationDialog.h
    src/ui/dialogs/ReportConfirmationDialog.cpp
    include/ui/dialogs/ReportSuccessDialog.h
    src/ui/dialogs/ReportSuccessDialog.cpp
    include/ui/DashboardLayout.h
    include/ui/panels/DashboardPanelFactory.h
    include/ui/panels/DefaultDashboardPanelFactory.h
    src/ui/panels/DefaultDashboardPanelFactory.cpp
    include/ui/panels/DashboardPanelCoordinator.h
    src/ui/panels/DashboardPanelCoordinator.cpp
    include/ui/panels/DeviceStatusPanel.h
    src/ui/panels/DeviceStatusPanel.cpp
    include/ui/panels/EventLogPanel.h
    src/ui/panels/EventLogPanel.cpp
    include/ui/panels/ObjectListPanel.h
    src/ui/panels/ObjectListPanel.cpp
    include/ui/DigitalTwinMapWidget.h
    src/ui/DigitalTwinMapWidget.cpp
    include/ui/DigitalTwinMapSceneBuilder.h
    src/ui/DigitalTwinMapSceneBuilder.cpp
    include/ui/DigitalTwinObjectStyleProvider.h
    src/ui/DigitalTwinObjectStyleProvider.cpp
    include/ui/DigitalTwinObjectTableModel.h
    src/ui/DigitalTwinObjectTableModel.cpp
    include/ui/EventLogTableModel.h
    src/ui/EventLogTableModel.cpp
    include/ui/VideoRiskBorderFrame.h
    src/ui/VideoRiskBorderFrame.cpp

    include/overlays/RadarPulseItem.h
    src/overlays/RadarPulseItem.cpp
    include/overlays/DangerAlertOverlay.h
    src/overlays/DangerAlertOverlay.cpp
    include/overlays/OverlayManager.h
    src/overlays/OverlayManager.cpp
    include/overlays/DeviceStatusMapOverlay.h
    src/overlays/DeviceStatusMapOverlay.cpp

    include/video/StreamReceiver.h
    include/video/StreamReceiverFactory.h
    include/video/VideoRuntimeConfig.h
    include/video/GstStreamReceiverFactory.h
    src/video/GstStreamReceiverFactory.cpp
    include/video/GstRtspReceiver.h
    src/video/GstRtspReceiver.cpp
    include/video/BlurProcessor.h
    src/video/BlurProcessor.cpp
    include/video/VideoUtcClockMapper.h
    src/video/VideoUtcClockMapper.cpp
    include/video/BlurVideoFilter.h
    src/video/BlurVideoFilter.cpp

    include/model/DigitalTwinTypes.h
    include/model/DigitalTwinMapDisplaySettings.h
    include/model/DeviceStatus.h
    include/model/DeviceStatusReport.h
    include/model/MqttRealtimeData.h
    include/model/RiskObjectTracker.h
    src/model/RiskObjectTracker.cpp
    include/model/DigitalTwinObjectSpawner.h
    src/model/RandomEdgeObjectSpawner.cpp
    include/model/DigitalTwinRiskPolicy.h
    include/model/DigitalTwinSimulationWorker.h
    src/model/DigitalTwinSimulationWorker.cpp
    src/model/DistanceRiskPolicy.cpp
    include/model/EventLogEntry.h
    include/model/EventLogGenerator.h
    src/model/EventLogGenerator.cpp
    include/model/StreamConfig.h

    include/network/DeviceStatusGateway.h
    src/network/DeviceStatusGateway.cpp
    include/network/ReportGateway.h
    src/network/ReportGateway.cpp
    include/network/SlackReportGateway.h
    src/network/SlackReportGateway.cpp
    include/model/ReportRequest.h
    include/network/DeviceStatusGatewayFactory.h
    include/network/MqttConnectionConfig.h
    include/network/MqttRuntimeConfig.h
    include/network/MqttSubscription.h
    include/network/MqttTransport.h
    include/network/MqttTransportFactory.h
    include/network/QtMqttTransport.h
    src/network/QtMqttTransport.cpp
    include/network/QtMqttTransportFactory.h
    src/network/QtMqttTransportFactory.cpp
    include/network/MqttTopicHandler.h
    include/network/MqttTopicFilter.h
    src/network/MqttTopicFilter.cpp
    include/network/MqttMessageRouter.h
    src/network/MqttMessageRouter.cpp
    include/network/DeviceStatusTopicHandler.h
    src/network/DeviceStatusTopicHandler.cpp
    include/network/RiskTopicHandler.h
    src/network/RiskTopicHandler.cpp
    include/network/BlurTopicHandler.h
    src/network/BlurTopicHandler.cpp
    include/network/MqttDeviceStatusGateway.h
    include/network/BlurMessageParser.h
    src/network/BlurMessageParser.cpp
    include/network/BlurFrameDispatcher.h
    src/network/BlurFrameDispatcher.cpp
    include/network/BlurFrameBuffer.h
    include/network/LatestBlurFrameBuffer.h
    src/network/LatestBlurFrameBuffer.cpp
    include/network/RiskFrameBuffer.h
    include/network/LatestRiskFrameBuffer.h
    src/network/LatestRiskFrameBuffer.cpp
    include/network/RiskMessageParser.h
    src/network/RiskMessageParser.cpp
    include/network/RiskFrameDispatcher.h
    src/network/RiskFrameDispatcher.cpp
    src/network/MqttDeviceStatusGateway.cpp
    include/network/MqttDeviceStatusGatewayFactory.h
    src/network/MqttDeviceStatusGatewayFactory.cpp
    include/network/DeviceStatusService.h
    src/network/DeviceStatusService.cpp
    include/video/StreamSessionManager.h
    src/video/StreamSessionManager.cpp
)

set(QTCCTV_LOCAL_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/app_config.json")
set(QTCCTV_EXAMPLE_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/app_config.example.json")
if(EXISTS "${QTCCTV_LOCAL_CONFIG}")
    set(QTCCTV_CONFIG_SOURCE "${QTCCTV_LOCAL_CONFIG}")
else()
    set(QTCCTV_CONFIG_SOURCE "${QTCCTV_EXAMPLE_CONFIG}")
endif()
configure_file("${QTCCTV_CONFIG_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/config/app_config.json" COPYONLY)

qt_add_resources(Qtcctvclient "style_resources"
    PREFIX "/styles"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/styles"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/styles/app.qss"
)

qt_add_resources(Qtcctvclient "icon_resources"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
    BIG_RESOURCES
    OPTIONS -no-compress
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/human.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/human_danger.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/human_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/human_warning.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/buzzer_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/cctv_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/config_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/danger_alert.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/led_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/led_off.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/led_safe.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/led_waring.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/led_danger.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/main.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/report_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/sensor_off.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/sensor_safe.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/sensor_danger.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/siren_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/vehicle.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/vehicle_danger.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/vehicle_icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/vehicle_warning.png"
)

target_include_directories(Qtcctvclient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(Qtcctvclient SYSTEM PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
)

ProcessorCount(QTCCTV_CPU_COUNT)

if(QTCCTV_CPU_COUNT)
    set_property(TARGET Qtcctvclient PROPERTY AUTOGEN_PARALLEL "${QTCCTV_CPU_COUNT}")
endif()

if(QTCCTV_ENABLE_PCH)
    target_precompile_headers(Qtcctvclient PRIVATE
        <algorithm>
        <memory>

        <QApplication>
        <QAbstractTableModel>
        <QBrush>
        <QColor>
        <QDebug>
        <QFrame>
        <QGraphicsItem>
        <QGraphicsPathItem>
        <QGraphicsScene>
        <QGraphicsSimpleTextItem>
        <QGraphicsView>
        <QLabel>
        <QMainWindow>
        <QObject>
        <QPainter>
        <QPainterPath>
        <QPen>
        <QPointF>
        <QRectF>
        <QSet>
        <QShowEvent>
        <QSize>
        <QSizePolicy>
        <QString>
        <QTableView>
        <QThread>
        <QTimer>
        <QTime>
        <QVector>
        <QWidget>
    )
endif()

if(QTCCTV_ENABLE_UNITY_BUILD)
    set_property(TARGET Qtcctvclient PROPERTY UNITY_BUILD ON)
    set_property(TARGET Qtcctvclient PROPERTY UNITY_BUILD_BATCH_SIZE 8)
endif()

find_program(SCCACHE_PROGRAM sccache)
find_program(CCACHE_PROGRAM ccache)

if(SCCACHE_PROGRAM)
    set_property(TARGET Qtcctvclient PROPERTY CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
elseif(CCACHE_PROGRAM)
    set_property(TARGET Qtcctvclient PROPERTY CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

target_link_directories(Qtcctvclient PRIVATE
    "${GSTREAMER_ROOT}/lib"
)

target_link_libraries(Qtcctvclient PRIVATE
    Qt6::Widgets
    Qt6::Mqtt
    Qt6::Network

    gstreamer-1.0
    gstbase-1.0
    gstrtsp-1.0
    gstvideo-1.0
    gobject-2.0
    glib-2.0
    intl
)
