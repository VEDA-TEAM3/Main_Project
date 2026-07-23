cmake_minimum_required(VERSION 3.16)

project(ComputeServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(
    MOSQUITTO
    REQUIRED
    IMPORTED_TARGET
    libmosquitto
)

add_executable(compute-server
    src/main.cpp
    src/core/AppContext.cpp
    src/core/Pipeline.cpp
    src/source/RtspOnvifSourceV2.cpp
    src/network/RtspClientV2.cpp
    src/parser/OnvifParser.cpp
    src/mapper/AffineImageCoordinateMapper.cpp
    src/sanitize/ContainmentSanitizer.cpp
    src/route/ParentBasedRouter.cpp
    src/ground/BottomCenterExtractor.cpp
    src/transform/HomographyTransform.cpp
    src/mqtt/MqttTransport.cpp
    src/sink/MqttBlurSink.cpp
    src/sink/MqttTopViewSink.cpp
)

target_include_directories(
    compute-server
    PRIVATE
        include
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(compute-server PRIVATE -O2)

target_link_libraries(
    compute-server
    PRIVATE
        OpenSSL::Crypto
        Threads::Threads
        PkgConfig::MOSQUITTO
)

# 로컬->월드 좌표 변환 검증 하네스.
# 변환 관련 소스만 실제로 링크하므로 mosquitto/OpenSSL 의존이 없음.
#   cmake --build build --target verify-world && ./build/verify-world
add_executable(verify-world
    test/verify_world.cpp
    src/transform/HomographyTransform.cpp
    src/ground/BottomCenterExtractor.cpp
)

target_include_directories(
    verify-world
    PRIVATE
        include
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(verify-world PRIVATE -O2)

target_link_libraries(verify-world PRIVATE Threads::Threads)

# Sink 수명/동시성 회귀 테스트.
# IMqttTransport 를 Fake 로 주입하므로 libmosquitto 없이도 Sink 로직을 검증함.
#   cmake --build build --target sink-lifecycle && ./build/sink-lifecycle
add_executable(sink-lifecycle
    test/sink_lifecycle.cpp
    src/sink/MqttTopViewSink.cpp
    src/sink/MqttBlurSink.cpp
)

target_include_directories(
    sink-lifecycle
    PRIVATE
        include
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(sink-lifecycle PRIVATE -O2)

target_link_libraries(sink-lifecycle PRIVATE Threads::Threads)