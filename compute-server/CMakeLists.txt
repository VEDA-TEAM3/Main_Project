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
    src/sink/MqttSink.cpp
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