cmake_minimum_required(VERSION 3.16)
project(parking_vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)

add_library(shared INTERFACE)
target_include_directories(shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/shared)
target_link_libraries(shared INTERFACE nlohmann_json::nlohmann_json)

# 기본값 ON -- CI 를 포함해 평소에는 두 서버를 모두 빌드한다.
# '컴파일이 깨지지 않는가'는 테스트 범위와 무관하게 저장소 전체에 대해 보장해야 하기 때문이다
# (control-server 는 아직 테스트가 없지만, 빌드는 CI 가 계속 지킨다)
# OFF 는 compute-server 만 필요한 엣지 배포 빌드용 탈출구다
option(VEDA_BUILD_CONTROL_SERVER "control-server 바이너리를 함께 빌드" ON)

add_subdirectory(compute-server)

if(VEDA_BUILD_CONTROL_SERVER)
    add_subdirectory(control-server)
endif()

# ---------------------------------------------------------------------------
# 테스트
#
# BUILD_TESTING 은 CTest 모듈이 제공하는 표준 스위치다.
# 라즈베리파이 배포 빌드처럼 GTest 를 내려받을 수 없는 환경에서는
#   cmake -B build -S . -DBUILD_TESTING=OFF
# 로 테스트 트리 전체(FetchContent 포함)를 건너뛴다
# ---------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    # GTest/GMock 은 시스템에 설치되어 있지 않아도 되도록 FetchContent 로 가져온다.
    # 예전에는 각자 소스에서 빌드해 -I/-L 로 직접 이어붙여야 했고, 그래서 단위 테스트가
    # cmake --build 에 아예 포함되지 않았다 (CI 에서 자동 실행이 불가능했던 원인)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )

    # Windows 전용 옵션이지만 지정해두면 크로스 플랫폼 경고가 줄어든다
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    # GTest 자체의 테스트/설치 타깃까지 우리 빌드에 섞이지 않도록 끈다
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()
