set(COMPUTE_DIR ${CMAKE_SOURCE_DIR}/compute-server)
set(CONTROL_DIR ${CMAKE_SOURCE_DIR}/control-server)

find_package(Threads REQUIRED)

# GTest 단위 테스트
add_subdirectory(compute-server/unit)
add_subdirectory(control-server/unit)