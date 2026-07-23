#pragma once

/**
 * @file    IMqttTransport.h
 * @brief   MQTT 브로커로의 단일 연결을 추상화한 전송 계층 인터페이스
 *
 * @details
 * Sink 는 '무엇을 언제 보낼지'(큐잉 정책, 유효성 검사, 토픽/QoS)를 정하고,
 * 이 인터페이스의 구현체는 '어떻게 보낼지'(TCP/TLS 연결, 재접속, mosquitto 루프)만 담당함
 *
 * @note [ 하나의 연결을 여러 Sink 가 공유하는 이유 ]
 * 예전에는 MqttBlurSink 와 MqttTopViewSink 가 각자 mosquitto 클라이언트를 만들어
 * 채널당 TLS 커넥션 2개 + mosquitto 네트워크 스레드 2개를 썼음 (4채널이면 커넥션 8개).
 * OpenSSL 세션 버퍼가 커넥션당 수십 KB라 라즈베리파이에서는 프레임 버퍼보다 비중이 컸고,
 * 두 Sink 가 각자 mosquitto_lib_init()/mosquitto_lib_cleanup() 을 부르는 바람에
 * 먼저 파괴된 쪽이 전역 상태를 해제해버리는 종료 시 UB 도 있었음
 * -> 연결을 하나로 모으고, 라이브러리 초기화는 참조 카운트로 관리
 *
 * @note [ 큐는 왜 공유하지 않는가 ]
 * 전송 큐와 워커 스레드는 Sink 마다 따로 둠
 * blur 쪽이 밀린다고 risk(안전 크리티컬) 발행이 함께 지연되면 안 되기 때문
 */

#include <cstdint>
#include <functional>
#include <string_view>

/**
 * @brief MQTT 발행 전송 계층
 */
class IMqttTransport {
public:
    /// @brief 등록된 리스너를 지목하는 핸들 (0 = 없음)
    using ListenerId = std::uint64_t;
    static constexpr ListenerId kInvalidListener = 0;

    virtual ~IMqttTransport() = default;

    /**
     * @brief   연결 상태가 바뀔 때 호출될 리스너를 등록
     *
     * @param   listener 연결됨(true)/끊김(false) 을 받는 콜백
     * @return  removeConnectionListener 에 넘길 핸들
     *
     * @warning [ 스레드 ]
     * mosquitto 네트워크 스레드에서 호출되므로 절대 블로킹하면 안 됨
     * (조건변수 notify 정도만 할 것)
     *
     * @note    start() 이전에 등록해야 최초 연결 이벤트를 놓치지 않음
     */
    virtual ListenerId addConnectionListener(std::function<void(bool)> listener) = 0;

    /**
     * @brief   리스너 등록을 해제 (멱등, kInvalidListener 는 무시)
     *
     * @details
     * 리스너는 보통 Sink 의 this 를 캡처하는데, Sink 는 transport 보다 먼저 파괴됨
     * -> 해제하지 않으면 이후의 연결/끊김 이벤트가 죽은 객체를 호출하게 됨
     *
     * @warning [ 반환 시점 보장 ]
     * 이 함수가 반환한 뒤에는 해당 리스너가 호출 중이지도, 앞으로 호출되지도 않음
     * (구현체는 콜백 실행 구간과 이 해제 구간을 상호 배제해야 함)
     */
    virtual void removeConnectionListener(ListenerId id) noexcept = 0;

    /**
     * @brief   클라이언트를 만들고 브로커 접속과 네트워크 루프를 시작
     * @details 최초 접속에 실패해도 백그라운드에서 재시도하므로 false 여도 치명적이지 않음
     * @return  즉시 접속 시도까지 성공했으면 true
     */
    virtual bool start() noexcept = 0;

    /**
     * @brief   네트워크 루프를 멈추고 클라이언트를 정리 (멱등)
     */
    virtual void stop() noexcept = 0;

    /**
     * @brief   페이로드 하나를 발행 (논블로킹)
     *
     * @param   topic   발행 토픽
     * @param   payload 페이로드 바이트
     * @param   qos     0..2
     * @param   retain  retained 여부
     * @return  mosquitto 가 큐잉을 받아들였으면 true
     *
     * @warning 예외를 던지지 않음. 실패는 false 로만 알림
     */
    virtual bool publish(std::string_view topic, std::string_view payload, int qos, bool retain = false) noexcept = 0;

    /// @brief 클라이언트가 만들어지고 루프가 도는 중인가 (브로커 접속 여부와는 별개)
    virtual bool isReady() const noexcept = 0;

    /// @brief 브로커와 실제로 연결되어 있는가
    virtual bool isConnected() const noexcept = 0;
};
