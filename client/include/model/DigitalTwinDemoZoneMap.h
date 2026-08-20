#pragma once

#include <algorithm>

#include "ui/DigitalTwinZoneIndex.h"

// ---------------------------------------------------------------------------
// **데모 시뮬레이션 전용 좌표 변환입니다. 실 데이터(topview) 경로는 이 파일을 쓰지 않습니다.**
//
// 실측 좌표는 서버가 실어 보낸 zoneId가 구역을 정하고, 구역 안 위치는 그 구역의 월드 상자로
// 정규화합니다(DigitalTwinMapWidget::planPointForObject의 live 분기). 화면에서 x로 구역을 다시
// 판정하면 서버 결과와 어긋나므로, 그 규칙을 이 파일이 건드리는 일은 없어야 합니다.
//
// 데모는 반대로 서버가 없으므로 좌표가 유일한 근거입니다. 활성 구역을 가로로 이어 붙인 띠
// 하나를 놓고 그 위에서 객체를 움직입니다. 전역 x는 [0, zoneCount] 범위이고 정수 한 칸이
// 구역 하나입니다. 화면에 넘길 때는 (구역 번호, 구역 안 0~1 좌표)로 다시 쪼갭니다 — 지도
// 위젯의 demo 분기가 그 형태를 기대하기 때문입니다.
//
// 채널 수 상수는 ui/DigitalTwinZoneIndex.h의 것을 그대로 씁니다. 여기에 4를 다시 적으면
// 언젠가 한쪽만 고쳐져 데모와 실 데이터의 구역 나눗셈이 어긋납니다.
// ---------------------------------------------------------------------------

/**
 * @brief          구역 정사각형 안의 위치가 네 채널 중 어디에 속하는지 판정합니다.
 * @param localX   구역 안 x (0~1, 경계 바깥에서는 조금 벗어날 수 있음)
 * @param localY   구역 안 y (0~1)
 * @return         0=위, 1=오른쪽, 2=아래, 3=왼쪽
 *
 * @details 카메라가 구역 한가운데서 네 방향을 보므로 채널 경계는 정사각형의 두 대각선입니다.
 *          ZoneStation.qml이 위험 표시를 그리는 사분면과 같은 나눗셈이라, 객체가 서 있는
 *          삼각형이 곧 그 객체의 채널이 됩니다.
 */
inline int digitalTwinDemoSectorForLocalPoint(double localX, double localY) {
    const bool belowMainDiagonal = localY >= localX;
    const bool belowAntiDiagonal = localY >= 1.0 - localX;

    if (!belowMainDiagonal && !belowAntiDiagonal) {
        return 0;
    }

    if (!belowMainDiagonal && belowAntiDiagonal) {
        return 1;
    }

    if (belowMainDiagonal && belowAntiDiagonal) {
        return 2;
    }

    return 3;
}

/**
 * @brief            띠 전체 기준 x가 몇 번째 구역에 있는지 반환합니다.
 * @param globalX    띠 전체 기준 x ([0, zoneCount], 진입·이탈 여백만큼 벗어날 수 있음)
 * @param zoneCount  활성 구역 수
 * @return           0부터 zoneCount - 1까지
 *
 * @details 여백 구간(음수, zoneCount 초과)은 양 끝 구역에 붙입니다. 그래야
 *          `구역 번호 + 구역 안 x`로 전역 x를 그대로 복원할 수 있어, 객체가 따로 전역
 *          좌표를 들고 다니지 않아도 됩니다. std::floor는 쓰지 않습니다 — 음수 쪽을
 *          먼저 걸러 내므로 정수 변환만으로 충분합니다.
 */
inline int digitalTwinDemoZoneForGlobalX(double globalX, int zoneCount) {
    const int lastZoneIndex = std::max(0, zoneCount - 1);

    if (globalX <= 0.0) {
        return 0;
    }

    if (globalX >= static_cast<double>(zoneCount)) {
        return lastZoneIndex;
    }

    return std::min(static_cast<int>(globalX), lastZoneIndex);
}

/**
 * @brief            띠 전체 기준 x에서 구역 안 x를 잘라 냅니다.
 * @param globalX    띠 전체 기준 x
 * @param zoneCount  활성 구역 수
 * @return           구역 안 x (양 끝 구역에서는 0~1을 여백만큼 벗어날 수 있음)
 */
inline double digitalTwinDemoLocalX(double globalX, int zoneCount) {
    return globalX - static_cast<double>(digitalTwinDemoZoneForGlobalX(globalX, zoneCount));
}

/**
 * @brief            객체가 지금 서 있는 자리로 채널 번호를 정합니다.
 * @param globalX    띠 전체 기준 x
 * @param localY     구역 안 y (0~1)
 * @param zoneCount  활성 구역 수
 * @return           구역과 사분면을 합친 채널 번호
 */
inline int digitalTwinDemoChannelIndex(double globalX, double localY, int zoneCount) {
    const int zoneIndex = digitalTwinDemoZoneForGlobalX(globalX, zoneCount);
    const double localX = globalX - static_cast<double>(zoneIndex);

    return zoneIndex * digitalTwinChannelsPerZone + digitalTwinDemoSectorForLocalPoint(localX, localY);
}
