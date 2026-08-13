cmake_minimum_required(VERSION 3.16)
project(parking_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)

add_library(shared INTERFACE)
target_include_directories(shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared)
target_link_libraries(shared INTERFACE nlohmann_json::nlohmann_json)

option(VEDA_BUILD_CONTROL_SERVER "control-server 바이너리를 함께 빌드" ON)

add_subdirectory(compute-server)

if(VEDA_BUILD_CONTROL_SERVER)
    add_subdirectory(control-server)
endif()

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()
