/**
 * @file    IAppSender.h
 * @brief   App으로 데이터를 전송하는 인터페이스
 * @note    전송 프로토콜이 아직 미정이므로 프로토콜 무관하게 추상화한다.
 *          구현체가 실제 전송 방식을 담당한다.
 * @note    sendTrackedFrame과 sendBlurTarget은 호출 주기와 목적이 다르므로 별도 메서드로
 *          분리한다. 하나로 묶으면 두 데이터가 같은 통신 경로로 뒤섞여,
 *          App이 수신 시점에 어떤 데이터인지 구분하기 위한 별도 처리가
 *          구현체 책임으로 명확히 드러나지 않는다.
 */
#pragma once

#include "model/outgoing/AppPacket.h"
#include "model/outgoing/BlurTarget.h"

/**
 * @brief   App 전송 인터페이스
 */
class IAppSender {
public:
    virtual ~IAppSender() = default;

    /**
     * @brief   frameIntervalMs 주기로 전체 TrackedPoint와 채널별 위험 레벨을 전송한다.
     * @param   packet   이번 프레임의 좌표 및 위험 판단 결과
     */
    virtual void sendTrackedFrame(const AppPacket& packet) = 0;

    /**
     * @brief   블러 처리 대상을 즉시 전송한다.
     * @note    FrameAggregator를 거치지 않고 IParser에서 파싱되는 즉시 호출된다.
     *          영상 프레임과의 실시간 매칭이 목적이므로 배치 처리하지 않는다.
     * @param   target   블러 처리 대상 (Face 또는 LicensePlate)
     */
    virtual void sendBlurTarget(const BlurTarget& target) = 0;
};