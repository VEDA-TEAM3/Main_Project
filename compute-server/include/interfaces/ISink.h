#pragma once

/**
 * @file ISink.h
 * @brief 프레임 하나를 하위 전송 계층으로 내보내는 싱크 인터페이스
 *
 * @note [스레드]
 * Pipeline 스레드에서 호출됨. 구현체는 반드시 논블로킹이어야 함.
 * 네트워크 I/O 는 내부 큐 + 자체 스레드로 처리할 것.
 * 여기서 블로킹하면 파이프라인 전체가 멈춤.
 *
 * @warning [실패 처리]
 * 절대 예외를 던지지 않음. 전송 불가 시 조용히 버리고 로그만 남김.
 * (MQTT: 브로커 끊김 / RTP: 구독자 없음 -- 둘 다 정상적으로 발생하는 상황)
 *
 * @warning [소유권]
 * send() 는 참조만 받음. 구현체가 비동기로 보관해야 하면 내부에서 복사할 것.
 *
 * @tparam T 전송할 프레임의 데이터 타입 (예: TopViewFrame, BlurFrame 등)
 */
template <typename T>
class ISink {
public:
    virtual ~ISink() = default;

    /**
     * @brief 프레임을 전송 계층으로 보냄. (논블로킹 필수)
     * @param frame 하위 계층으로 보낼 데이터 프레임
     */
    virtual void send(const T& frame) = 0;
};