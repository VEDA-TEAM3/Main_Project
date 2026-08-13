#pragma once

/// 물리 CCTV 한 대가 연속된 채널 4개를 쓴다. zoneId 0~3은 첫 번째 Scene, 4~7은 두 번째 Scene이다
constexpr int digitalTwinChannelsPerZone = 4;
constexpr int digitalTwinChannelCount = 8;

/**
 * @brief               객체를 그릴 물리 CCTV Scene 인덱스를 정합니다.
 * @param channelIndex  서버가 실어 보낸 zoneId (유효 범위를 벗어나면 -1)
 * @param worldX        객체의 월드 좌표 x
 * @param worldCenterX  두 Scene을 가르는 월드 좌표 경계
 * @return              0 또는 1
 *
 * @details 서버 zoneId는 최근접 CCTV 선택과 히스테리시스를 이미 거친 권위 있는 결과다.
 * 화면에서 x로 다시 판정하면 경계(x=0) 근처에서 서버 결과와 어긋나, 같은 gid가 두 Scene을
 * 왕복하고 아이콘과 레이더 파동이 서로 다른 맵에 그려진다. zoneId가 없을 때(-1)만 좌표로
 * 되돌아간다.
 */
inline int digitalTwinZoneIndex(int channelIndex, double worldX, double worldCenterX) {
    if (channelIndex >= 0 && channelIndex < digitalTwinChannelCount) {
        return channelIndex / digitalTwinChannelsPerZone;
    }

    return worldX <= worldCenterX ? 0 : 1;
}
