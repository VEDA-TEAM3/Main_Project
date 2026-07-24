cmake_minimum_required(VERSION 3.16)
project(ControlServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

add_executable(control-server
    src/main.cpp
    src/core/AppContext.cpp
    src/core/Controller.cpp
    src/receive/MqttChannelReceiver.cpp
    src/aggregate/TimeWindowAggregatorV2.cpp
    src/time/SystemClock.cpp
    src/fuse/ConcatFuser.cpp
    src/fuse/GridFuser.cpp
    src/metric/EuclideanMetric.cpp
    src/risk/ThresholdRiskPolicy.cpp
    src/dispatch/SerialHwEventDispatcher.cpp
    src/sink/MqttTransport.cpp
    src/zone/SpatialZoneMapper.cpp
    src/transform/AffineLocalToWorldTransform.cpp
)

target_include_directories(control-server PRIVATE 
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(control-server PRIVATE -O2)

target_link_libraries(control-server PRIVATE
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MOSQUITTO
)

# 좌표 변환 / zone 배정 / 위험 판정 / 채널 융합 검증 하네스.
# 순수 계산 경로만 링크하므로 mosquitto/OpenSSL 의존이 없음.
#   cmake --build build --target verify-pipeline && ./build/verify-pipeline
add_executable(verify-pipeline
    test/verify_pipeline.cpp
    src/transform/AffineLocalToWorldTransform.cpp
    src/zone/SpatialZoneMapper.cpp
    src/risk/ThresholdRiskPolicy.cpp
    src/fuse/ConcatFuser.cpp
    src/metric/EuclideanMetric.cpp
)

target_include_directories(verify-pipeline PRIVATE
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(verify-pipeline PRIVATE -O2)

target_link_libraries(verify-pipeline PRIVATE Threads::Threads)