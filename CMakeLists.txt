cmake_minimum_required(VERSION 3.16)
project(parking_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(shared INTERFACE)
target_include_directories(shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared)
target_link_libraries(shared INTERFACE nlohmann_json::nlohmann_json)

add_subdirectory(compute-server)
add_subdirectory(control-server)