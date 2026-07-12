cmake_minimum_required(VERSION 3.10)
project(Controller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

# 내부 구현이 바뀌었다면 이 부분을 수정
add_library(controller_core
    src/core/AppContext.cpp
    src/core/Controller.cpp
    src/core/FrameAggregator.cpp
)

target_include_directories(controller_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(controller_core
    PUBLIC
        Threads::Threads
        nlohmann_json::nlohmann_json
        ${CMAKE_DL_LIBS}
)

add_executable(controller_main
    src/main.cpp
)

target_link_libraries(controller_main
    PRIVATE
        controller_core
)