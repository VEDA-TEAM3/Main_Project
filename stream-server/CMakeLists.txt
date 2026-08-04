# stream-server — RTSP 수집 → 롤링 DVR + RTSP 중계 (무디코드)
#
# 의존성 설치 (Debian/Ubuntu ARM64, Raspberry Pi OS 64-bit):
#   sudo apt install -y \
#       libgstreamer1.0-dev \
#       libgstreamer-plugins-base1.0-dev \
#       libgstrtspserver-1.0-dev \
#       gstreamer1.0-plugins-base \
#       gstreamer1.0-plugins-good \
#       gstreamer1.0-rtsp
#
# 런타임 플러그인이 어디서 오는지 (없으면 gst_parse_launch 가 런타임에 실패한다):
#   rtspsrc, rtpjitterbuffer .......... gstreamer1.0-plugins-good (rtsp, rtpmanager)
#   rtph264depay / rtph264pay ......... gstreamer1.0-plugins-good (rtp)
#   h264parse / h265parse ............. gstreamer1.0-plugins-bad  (videoparsersbad)
#   splitmuxsink, matroskamux, qtmux .. gstreamer1.0-plugins-good (multifile, matroska, isomp4)
#   appsrc / appsink .................. gstreamer1.0-plugins-base (app)
#
# ⚠️ h264parse 는 plugins-bad 에 있다. 이것 하나 때문에 bad 가 필요하며,
#    빼면 파이프라인이 "no element h264parse" 로 런타임에만 죽는다 (빌드는 통과한다):
#   sudo apt install -y gstreamer1.0-plugins-bad
#
# ⚠️ gstreamer1.0-libav 는 **설치하지 않는다.** 디코더가 깔려 있으면 누군가 파이프라인에
#    decodebin 을 끼워 넣게 되고, 그 순간 4채널 CPU 예산이 무너진다. 없는 편이 안전하다.

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.18
    gstreamer-app-1.0>=1.18
    gstreamer-rtsp-server-1.0>=1.18
)

# src/network/ 와 src/core/Pipeline.cpp 은 만들지 않았다. 재연결은 RtspSource 가 GstBus
# 워치로 직접 처리하고, '파이프라인 단계'는 GStreamer 그래프 자체다 -- 그 위에 우리 쪽
# 단계 추상화를 한 겹 더 올리면 지연만 늘고 얻는 것이 없다.
add_executable(stream-server
    src/main.cpp
    src/core/AppContext.cpp
    src/source/RtspSource.cpp
    src/storage/RollingRecorder.cpp
    src/sink/RtspRelaySink.cpp
)

target_include_directories(stream-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(stream-server PRIVATE
    shared                  # Contract.h / Logger.h — 세 서버가 공유
    PkgConfig::GST          # pkg_check_modules(GST ... IMPORTED_TARGET) 가 만드는 이름
    Threads::Threads
)

target_compile_features(stream-server PRIVATE cxx_std_20)

# GStreamer 헤더는 -Wold-style-cast 류에 깨끗하지 않다. 우리 코드에만 엄격 옵션을 건다.
target_compile_options(stream-server PRIVATE
    -Wall -Wextra -Wpedantic
)

install(TARGETS stream-server RUNTIME DESTINATION bin)
