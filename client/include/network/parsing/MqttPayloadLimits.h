#pragma once

#include <QtGlobal>

/**
 * @brief MQTT 외부 입력의 공통 상한과 시각 창.
 *
 * @details 브로커에 발행 ACL이 없으면 어떤 발행자든 이 클라이언트의 지도, 장비 패널,
 *          영상 블러에 직접 입력을 넣을 수 있다. 상한은 파서와 gateway가 함께 쓰므로
 *          한곳에 모아 둔다.
 */

/// MQTT 한 패킷은 규약상 256MB까지 가능하다. 정상 payload는 risk 프레임이 수십 KB,
/// blur가 수 KB이므로 이 상한은 실사용의 열 배 이상이다. 초과분은 잘라 쓰지 않고
/// 메시지 전체를 버린다 (잘라 쓰면 안전 판단에서 객체가 조용히 빠진다)
inline constexpr qsizetype maximumMqttPayloadBytes = 256 * 1024;

/// 한 RiskFrame이 실을 수 있는 융합 객체 수. 구역 8개 x 채널 4개 기준 실사용은 수십 개다
inline constexpr qsizetype maximumRiskObjectsPerFrame = 256;

/// payload의 ts가 로컬 UTC보다 이만큼 앞서면 거부한다.
///
/// 이 값은 blur.historyMs(기본 10s)보다 작아야 한다. BlurProcessor는 지금까지 본 가장
/// 최신 ts를 기준으로 그보다 historyMs 이상 과거인 metadata를 버리므로, 미래 ts를 한 번
/// 받아들이면 뒤이어 오는 정상 metadata가 전부 버려지고 블러가 조용히 꺼진다.
inline constexpr qint64 maximumFutureTimestampMsec = 5000;

/// payload의 ts가 로컬 UTC보다 이만큼 뒤처지면 거부한다. 실제 만료 판정은 각 소비자의
/// 이력(BlurProcessor history, RiskObjectTracker 보존 시간)이 하므로 여기서는 넉넉히 둔다
inline constexpr qint64 maximumPastTimestampMsec = 60000;

/**
 * @brief                 payload의 원본 시각이 로컬 시계와 맞물리는지 봅니다.
 * @param timestampMsec   payload의 ts (Unix epoch millisecond)
 * @param nowMsec         비교 기준으로 쓸 로컬 UTC millisecond
 * @return                허용 창 안이면 true
 *
 * @details 이 검사는 새 요구사항이 아니다. 블러는 서버 ts를 영상 PTS에 UTC로 대응시켜
 *          동작하므로 두 시계가 이미 초 단위로 맞아 있어야 한다. 맞지 않으면 블러가
 *          조용히 빠지는 대신 여기서 프로토콜 오류로 드러난다.
 */
inline bool isFreshSourceTimestamp(qint64 timestampMsec, qint64 nowMsec) {
    return timestampMsec > 0 && timestampMsec - nowMsec <= maximumFutureTimestampMsec &&
           nowMsec - timestampMsec <= maximumPastTimestampMsec;
}
