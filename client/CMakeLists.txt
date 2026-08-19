cmake_minimum_required(VERSION 3.16)

project(Qtcctvclient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ProcessorCount)

find_package(Qt6 REQUIRED COMPONENTS Mqtt Network Quick QuickControls2 QuickWidgets Widgets)

set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/mingw_x86_64" CACHE PATH
    "GStreamer MinGW x86_64 runtime root")

option(QTCCTV_ENABLE_PCH "Enable precompiled headers for faster Qt rebuilds" ON)
option(QTCCTV_ENABLE_UNITY_BUILD "Enable unity build for faster full rebuilds" OFF)
option(QTCCTV_BUILD_CHECKS "Build standalone logic self-checks" OFF)

qt_standard_project_setup()

qt_add_executable(Qtcctvclient
    "${CMAKE_CURRENT_SOURCE_DIR}/../.clang-format"
    "${CMAKE_CURRENT_SOURCE_DIR}/../.clang-tidy"

    src/main.cpp

    include/config/ApplicationConfig.h
    src/config/ApplicationConfig.cpp

    include/ui/mainwindow.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.ui
    include/ui/ClickableVideoWidget.h
    src/ui/ClickableVideoWidget.cpp
    include/auth/AuthGateway.h
    include/auth/LocalFileAuthGateway.h
    src/auth/LocalFileAuthGateway.cpp

    include/ui/dialogs/MapSettingsDialog.h
    src/ui/dialogs/MapSettingsDialog.cpp
    include/ui/dialogs/LoginWindow.h
    src/ui/dialogs/LoginWindow.cpp
    include/ui/dialogs/InformationDialog.h
    src/ui/dialogs/InformationDialog.cpp
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
    include/ui/SharedQmlEngine.h
    include/ui/TableModelRoles.h
    include/ui/DigitalTwinMapWidget.h
    src/ui/DigitalTwinMapWidget.cpp
    include/ui/DigitalTwinMapSceneBuilder.h
    src/ui/DigitalTwinMapSceneBuilder.cpp
    include/ui/DigitalTwinObjectStyleProvider.h
    src/ui/DigitalTwinObjectStyleProvider.cpp
    include/ui/DigitalTwinHeading.h
    include/ui/DigitalTwinZoneIndex.h
    include/ui/DigitalTwinObjectTableModel.h
    src/ui/DigitalTwinObjectTableModel.cpp
    include/ui/EventLogTableModel.h
    src/ui/EventLogTableModel.cpp
    include/ui/VideoRiskBorderFrame.h
    src/ui/VideoRiskBorderFrame.cpp

    include/overlays/DangerBorderOverlay.h
    src/overlays/DangerBorderOverlay.cpp
    include/overlays/ChannelRiskOverlay.h
    src/overlays/ChannelRiskOverlay.cpp
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
    include/model/DigitalTwinRuntimeConfig.h
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
    include/model/VideoPreprocessingSettings.h

    include/network/gateways/DeviceStatusGateway.h
    src/network/gateways/DeviceStatusGateway.cpp
    include/network/gateways/ReportGateway.h
    src/network/gateways/ReportGateway.cpp
    include/network/gateways/SlackReportGateway.h
    src/network/gateways/SlackReportGateway.cpp
    include/model/ReportRequest.h
    include/network/gateways/DeviceStatusGatewayFactory.h
    include/network/transport/MqttConnectionConfig.h
    include/network/transport/MqttRuntimeConfig.h
    include/network/transport/MqttSubscription.h
    include/network/transport/MqttTransport.h
    include/network/transport/MqttTransportFactory.h
    include/network/transport/QtMqttTransport.h
    src/network/transport/QtMqttTransport.cpp
    include/network/transport/QtMqttTransportFactory.h
    src/network/transport/QtMqttTransportFactory.cpp
    include/network/routing/MqttTopicHandler.h
    include/network/routing/MqttTopicFilter.h
    src/network/routing/MqttTopicFilter.cpp
    include/network/routing/MqttMessageRouter.h
    src/network/routing/MqttMessageRouter.cpp
    include/network/routing/DeviceStatusTopicHandler.h
    src/network/routing/DeviceStatusTopicHandler.cpp
    include/network/routing/RiskTopicHandler.h
    src/network/routing/RiskTopicHandler.cpp
    include/network/routing/BlurTopicHandler.h
    src/network/routing/BlurTopicHandler.cpp
    include/network/gateways/MqttDeviceStatusGateway.h
    include/network/parsing/BlurMessageParser.h
    include/network/parsing/MqttPayloadLimits.h
    src/network/parsing/BlurMessageParser.cpp
    include/network/realtime/BlurFrameDispatcher.h
    src/network/realtime/BlurFrameDispatcher.cpp
    include/network/realtime/BlurFrameBuffer.h
    include/network/realtime/LatestBlurFrameBuffer.h
    src/network/realtime/LatestBlurFrameBuffer.cpp
    include/network/realtime/RiskFrameBuffer.h
    include/network/realtime/LatestRiskFrameBuffer.h
    src/network/realtime/LatestRiskFrameBuffer.cpp
    include/network/parsing/RiskMessageParser.h
    src/network/parsing/RiskMessageParser.cpp
    include/network/realtime/RiskFrameDispatcher.h
    src/network/realtime/RiskFrameDispatcher.cpp
    src/network/gateways/MqttDeviceStatusGateway.cpp
    include/network/gateways/MqttDeviceStatusGatewayFactory.h
    src/network/gateways/MqttDeviceStatusGatewayFactory.cpp
    include/network/services/DeviceStatusService.h
    src/network/services/DeviceStatusService.cpp
    include/video/StreamSessionManager.h
    src/video/StreamSessionManager.cpp
)

set(QTCCTV_LOCAL_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/app_config.json")
set(QTCCTV_EXAMPLE_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/app_config.example.json")
if(EXISTS "${QTCCTV_LOCAL_CONFIG}")
    set(QTCCTV_CONFIG_SOURCE "${QTCCTV_LOCAL_CONFIG}")
elseif(EXISTS "${QTCCTV_EXAMPLE_CONFIG}")
    set(QTCCTV_CONFIG_SOURCE "${QTCCTV_EXAMPLE_CONFIG}")
else()
    message(FATAL_ERROR "Neither config/app_config.json nor config/app_config.example.json exists")
endif()
# 빌드 디렉터리 사본이 앱이 실제로 읽고 쓰는 설정이다. 설정 팝업의 구역 추가가 여기에 쓰므로
# 재구성 때마다 무조건 덮으면 사용자가 추가한 구역과 RTSP 계정이 조용히 사라진다.
# 원본이 더 새로울 때만(= 개발자가 직접 고쳤을 때만) 다시 복사한다.
set(QTCCTV_RUNTIME_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/config/app_config.json")
# configure_file은 "실행됐을 때만" 원본을 재구성 의존성으로 등록한다. 사본이 최신이라 아래 if가
# 거짓이면 등록이 통째로 빠지고, 그 뒤로는 원본을 고쳐도 재구성이 걸리지 않아 사본이 영원히
# 낡은 채로 남는다(설정을 고쳐도 앱이 안 바뀌는 원인). 등록만 항상 해 둔다.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${QTCCTV_CONFIG_SOURCE}")
if(NOT EXISTS "${QTCCTV_RUNTIME_CONFIG}" OR "${QTCCTV_CONFIG_SOURCE}" IS_NEWER_THAN "${QTCCTV_RUNTIME_CONFIG}")
    configure_file("${QTCCTV_CONFIG_SOURCE}" "${QTCCTV_RUNTIME_CONFIG}" COPYONLY)
endif()

qt_add_resources(Qtcctvclient "style_resources"
    PREFIX "/styles"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/styles"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/styles/app.qss"
)

qt_add_resources(Qtcctvclient "qml_resources"
    PREFIX "/qml"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Theme.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NeonButton.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PanelFrame.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PanelHeader.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/CctvToolbar.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/StatusLegend.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OverlayDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/GuideDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/DataTable.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/DeviceStatusView.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/SettingsDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/LoginScreen.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OptionCheckBox.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OptionComboBox.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OptionSlider.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OptionTextField.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/StatusLabel.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/TopBar.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/TopBarForm.ui.qml"
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
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickWidgets

    gstreamer-1.0
    gstbase-1.0
    gstrtsp-1.0
    gstvideo-1.0
    gobject-2.0
    glib-2.0
    intl
)

if(QTCCTV_BUILD_CHECKS)
    add_executable(risk_object_tracker_bounds_check
        tests/RiskObjectTrackerBoundsCheck.cpp
        src/model/RiskObjectTracker.cpp
    )

    target_include_directories(risk_object_tracker_bounds_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(risk_object_tracker_bounds_check PRIVATE Qt6::Gui)

    add_executable(digital_twin_heading_check
        tests/DigitalTwinHeadingCheck.cpp
    )

    target_include_directories(digital_twin_heading_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(digital_twin_heading_check PRIVATE Qt6::Gui)

    add_executable(table_model_roles_check
        tests/TableModelRolesCheck.cpp
    )

    target_include_directories(table_model_roles_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(table_model_roles_check PRIVATE Qt6::Gui)

    add_executable(config_area_writer_check
        tests/ConfigAreaWriterCheck.cpp
        src/config/ApplicationConfig.cpp
    )

    target_include_directories(config_area_writer_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_compile_definitions(config_area_writer_check PRIVATE
        QTCCTV_EXAMPLE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/app_config.example.json"
    )

    target_link_libraries(config_area_writer_check PRIVATE Qt6::Core)

    add_executable(local_file_auth_gateway_check
        tests/LocalFileAuthGatewayCheck.cpp
        src/auth/LocalFileAuthGateway.cpp
    )

    target_include_directories(local_file_auth_gateway_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(local_file_auth_gateway_check PRIVATE Qt6::Core Qt6::Network)

    add_executable(risk_message_parser_check
        tests/RiskMessageParserCheck.cpp
        src/network/parsing/RiskMessageParser.cpp
    )

    target_include_directories(risk_message_parser_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(risk_message_parser_check PRIVATE Qt6::Gui)

    add_executable(video_utc_clock_mapper_check
        tests/VideoUtcClockMapperCheck.cpp
        src/video/VideoUtcClockMapper.cpp
    )

    target_include_directories(video_utc_clock_mapper_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_include_directories(video_utc_clock_mapper_check SYSTEM PRIVATE
        "${GSTREAMER_ROOT}/include/gstreamer-1.0"
        "${GSTREAMER_ROOT}/include/glib-2.0"
        "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    )

    target_link_directories(video_utc_clock_mapper_check PRIVATE
        "${GSTREAMER_ROOT}/lib"
    )

    target_link_libraries(video_utc_clock_mapper_check PRIVATE
        Qt6::Core
        gstreamer-1.0
        gobject-2.0
        glib-2.0
        intl
    )

    add_executable(blur_processor_nv12_check
        tests/BlurProcessorNv12Check.cpp
        src/video/BlurProcessor.cpp
        src/video/VideoUtcClockMapper.cpp
    )

    target_include_directories(blur_processor_nv12_check PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_include_directories(blur_processor_nv12_check SYSTEM PRIVATE
        "${GSTREAMER_ROOT}/include/gstreamer-1.0"
        "${GSTREAMER_ROOT}/include/glib-2.0"
        "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    )

    target_link_directories(blur_processor_nv12_check PRIVATE
        "${GSTREAMER_ROOT}/lib"
    )

    target_link_libraries(blur_processor_nv12_check PRIVATE
        Qt6::Gui
        gstreamer-1.0
        gstvideo-1.0
        gobject-2.0
        glib-2.0
        intl
    )
endif()
