// 데모 시뮬레이션의 구역·채널 좌표 변환 자체 검사.
//
// 검사하는 것은 두 가지다.
//   1) 구역 정사각형 안의 자리가 네 채널 중 맞는 사분면으로 떨어지는가
//   2) 띠 전체 기준 x -> (구역, 구역 안 x) 쪼개기가 **되돌릴 수 있는가**
//
// 두 번째가 핵심이다. 객체는 전역 좌표를 따로 들고 다니지 않고 (구역 번호, 구역 안 x)만
// 저장하므로, 되돌리기가 깨지면 매 tick마다 위치가 조금씩 밀려 객체가 지도 위를 기어간다.

#include <cstdio>
#include <cstdlib>

#include "model/DigitalTwinDemoZoneMap.h"

namespace {

int failureCount = 0;

void expect(bool condition, const char* what) {
    if (condition) {
        return;
    }

    std::printf("FAIL: %s\n", what);
    ++failureCount;
}

void expectEqual(int actual, int expected, const char* what) {
    if (actual == expected) {
        return;
    }

    std::printf("FAIL: %s (expected %d, got %d)\n", what, expected, actual);
    ++failureCount;
}

/** 구역 사각형의 네 사분면이 각각 위·오른쪽·아래·왼쪽으로 나뉜다 */
void checkSectors() {
    expectEqual(digitalTwinDemoSectorForLocalPoint(0.5, 0.1), 0, "위쪽 사분면");
    expectEqual(digitalTwinDemoSectorForLocalPoint(0.9, 0.5), 1, "오른쪽 사분면");
    expectEqual(digitalTwinDemoSectorForLocalPoint(0.5, 0.9), 2, "아래쪽 사분면");
    expectEqual(digitalTwinDemoSectorForLocalPoint(0.1, 0.5), 3, "왼쪽 사분면");

    // 진입·이탈 여백에서는 구역 안 x가 0~1을 조금 벗어난다. 그때도 판정이 서야 한다
    expectEqual(digitalTwinDemoSectorForLocalPoint(-0.02, 0.5), 3, "왼쪽 여백");
    expectEqual(digitalTwinDemoSectorForLocalPoint(1.02, 0.5), 1, "오른쪽 여백");
}

/** 띠 위의 x가 어느 구역에 속하는지 */
void checkZoneSplit() {
    expectEqual(digitalTwinDemoZoneForGlobalX(0.6, 2), 0, "첫 구역 안");
    expectEqual(digitalTwinDemoZoneForGlobalX(1.4, 2), 1, "둘째 구역 안");
    expectEqual(digitalTwinDemoZoneForGlobalX(-0.02, 2), 0, "왼쪽 여백은 첫 구역에 붙는다");
    expectEqual(digitalTwinDemoZoneForGlobalX(2.02, 2), 1, "오른쪽 여백은 마지막 구역에 붙는다");
    expectEqual(digitalTwinDemoZoneForGlobalX(2.0, 2), 1, "띠 끝은 마지막 구역");
    expectEqual(digitalTwinDemoZoneForGlobalX(0.5, 1), 0, "구역이 하나뿐이면 언제나 0");
    expectEqual(digitalTwinDemoZoneForGlobalX(5.5, 6), 5, "구역 여섯 개의 마지막 칸");
}

/**
 * 쪼갠 뒤 다시 더하면 원래 x가 나와야 한다. 객체는 이 되돌리기에 기대어 전역 좌표 없이
 * 움직이므로, 여기서 어긋나면 이동이 매 tick 밀린다.
 */
void checkRoundTrip() {
    const int zoneCounts[] = {1, 2, 3, 6};
    for (const int zoneCount : zoneCounts) {
        for (int step = -2; step <= zoneCount * 100 + 2; ++step) {
            const double globalX = static_cast<double>(step) / 100.0;
            const int zoneIndex = digitalTwinDemoZoneForGlobalX(globalX, zoneCount);
            const double localX = digitalTwinDemoLocalX(globalX, zoneCount);
            const double restored = static_cast<double>(zoneIndex) + localX;

            expect(zoneIndex >= 0 && zoneIndex < zoneCount, "구역 번호가 범위 안이다");
            expect(restored > globalX - 1e-9 && restored < globalX + 1e-9, "쪼갠 값을 되돌리면 원래 x가 된다");
        }
    }
}

/** 채널 번호는 구역과 사분면을 합친 값이다 */
void checkChannelIndex() {
    expectEqual(digitalTwinDemoChannelIndex(0.5, 0.1, 2), 0, "1구역 위 채널");
    expectEqual(digitalTwinDemoChannelIndex(0.9, 0.5, 2), 1, "1구역 오른쪽 채널");
    expectEqual(digitalTwinDemoChannelIndex(1.5, 0.1, 2), digitalTwinChannelsPerZone + 0, "2구역 위 채널");
    expectEqual(digitalTwinDemoChannelIndex(1.1, 0.5, 2), digitalTwinChannelsPerZone + 3, "2구역 왼쪽 채널");

    // 구역 경계를 넘는 순간 채널이 다음 구역으로 옮겨 간다 — 이것이 이번 변경의 요점이다
    const int beforeBoundary = digitalTwinDemoChannelIndex(0.99, 0.5, 2);
    const int afterBoundary = digitalTwinDemoChannelIndex(1.01, 0.5, 2);
    expectEqual(beforeBoundary / digitalTwinChannelsPerZone, 0, "경계 직전에는 1구역");
    expectEqual(afterBoundary / digitalTwinChannelsPerZone, 1, "경계 직후에는 2구역");
}

}  // namespace

int main() {
    checkSectors();
    checkZoneSplit();
    checkRoundTrip();
    checkChannelIndex();

    if (failureCount > 0) {
        std::printf("%d check(s) failed\n", failureCount);
        return EXIT_FAILURE;
    }

    std::printf("demo zone/channel mapping OK\n");
    return EXIT_SUCCESS;
}
