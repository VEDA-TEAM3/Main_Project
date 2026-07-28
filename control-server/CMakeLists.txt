cmake_minimum_required(VERSION 3.16)
# C 는 verify-downlink 의 디코더(test/downlink_decoder.c)용 -- STM32 와 동일한 컴파일러
# 계열로 구조체 레이아웃을 교차 검증하기 위해 C 를 함께 활성화한다.
project(ControlServer LANGUAGES CXX C)

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

# AppConfig 의 하드웨어 채널/zone 범위 검증 하네스.
add_executable(verify-config
    test/verify_config.cpp
)

target_include_directories(verify-config PRIVATE
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(verify-config PRIVATE -O2)
target_link_libraries(verify-config PRIVATE nlohmann_json::nlohmann_json)

# UART 이벤트 채널/거리 인코딩 경계값 검증 하네스.
add_executable(verify-serial-encoding
    test/verify_serial_encoding.cpp
)

target_include_directories(verify-serial-encoding PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(verify-serial-encoding PRIVATE -O2)

# 하행(dispatch) 프레임 검증 하네스.
# pty 를 가짜 시리얼 포트로 물려 실제 write() 바이트를 캡처하고, C 컴파일러로 빌드한
# 디코더로 되읽어 STM32(C)/RPi(C++) 구조체 레이아웃 일치까지 함께 검증한다.
# Logger 가 헤더 온리라 mosquitto/OpenSSL 의존 없이 dispatcher 만 링크하면 된다.
#   cmake --build build --target verify-downlink && ./build/control-server/verify-downlink
add_executable(verify-downlink
    test/verify_downlink.cpp
    test/downlink_decoder.c
    src/dispatch/SerialHwEventDispatcher.cpp
)

target_include_directories(verify-downlink PRIVATE
    include
    src
    test
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

set_target_properties(verify-downlink PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(verify-downlink PRIVATE -O2)
target_link_libraries(verify-downlink PRIVATE Threads::Threads)
