cmake_minimum_required(VERSION 3.16)
project(ControlServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Threads REQUIRED)

add_executable(control-server
    src/main.cpp
    src/core/AppContext.cpp
    src/core/Controller.cpp
    src/receive/NullReceiver.cpp
    src/aggregate/TimeWindowAggregator.cpp
    src/time/SystemClock.cpp
    src/fuse/ConcatFuser.cpp
    src/metric/EuclideanMetric.cpp
    src/risk/ThresholdRiskPolicy.cpp
    src/dispatch/ConsoleDispatcher.cpp
    src/sink/ConsoleSink.cpp
    src/zone/NullZoneMapper.cpp
)

target_include_directories(control-server PRIVATE 
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

target_link_libraries(control-server PRIVATE
    Threads::Threads
)