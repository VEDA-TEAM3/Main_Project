cmake_minimum_required(VERSION 3.10)
project(Processor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

find_package(OpenSSL REQUIRED)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# 내부 구현이 바뀌었다면 이 부분을 수정
add_library(processor_core
    src/network/RtspClient.cpp
    src/parser/OnvifParser.cpp
    src/trans/HomographyTrans.cpp
    src/send/NullSender.cpp
    src/core/AppContext.cpp
    src/core/Processor.cpp
)

target_include_directories(processor_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(processor_core
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        ${OpenCV_LIBS}
        Threads::Threads
)

add_executable(processor_main
    src/main.cpp
)

target_link_libraries(processor_main
    PRIVATE
        processor_core
)