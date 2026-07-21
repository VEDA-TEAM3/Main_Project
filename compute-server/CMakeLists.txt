cmake_minimum_required(VERSION 3.16)
project(ComputeServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

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
)

target_include_directories(compute-server PRIVATE 
    include 
    src 
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_compile_options(compute-server PRIVATE -O2)

target_link_libraries(compute-server PRIVATE
    OpenSSL::Crypto
    Threads::Threads
)