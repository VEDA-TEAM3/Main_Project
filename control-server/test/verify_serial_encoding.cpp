/**
 * @file    verify_serial_encoding.cpp
 * @brief   UART 위험 이벤트 채널/거리 인코딩 경계값 검증 하네스
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <cstdio>
#include <limits>
#include <string>

#include "dispatch/SerialEventEncoding.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

}  // namespace

int main() {
    check(!serial_event::isValidChannelId(-1), "zoneId -1 은 UART 채널로 변환하지 않음");
    check(serial_event::isValidChannelId(0), "zoneId 0 은 유효");
    check(serial_event::isValidChannelId(255), "zoneId 255 는 uint8_t 최대 유효값");
    check(!serial_event::isValidChannelId(256), "zoneId 256 은 UART 채널로 변환하지 않음");

    check(serial_event::encodeDistanceMm(-1.0) == VEDA_DIST_MM_NONE, "음수 거리는 값 없음 sentinel");
    check(serial_event::encodeDistanceMm(std::numeric_limits<double>::quiet_NaN()) == VEDA_DIST_MM_NONE,
          "NaN 거리는 값 없음 sentinel");
    check(serial_event::encodeDistanceMm(std::numeric_limits<double>::infinity()) == VEDA_DIST_MM_NONE,
          "무한대 거리는 값 없음 sentinel");
    check(serial_event::encodeDistanceMm(0.0) == 0, "0m 는 유효한 0mm");
    check(serial_event::encodeDistanceMm(65.534) == 65534, "65.534m 는 최대 유효값 65534mm");
    check(serial_event::encodeDistanceMm(65.535) == 65534, "65.535m 는 sentinel 대신 65534mm로 포화");
    check(serial_event::encodeDistanceMm(1000.0) == 65534, "표현 범위 초과 거리는 65534mm로 포화");

    std::printf("\nPASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
