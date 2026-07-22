#pragma once

/**
 * @file    INetwork.h
 * @brief   CCTV와의 연결 인터페이스
 */

#include <functional>
#include <string_view>

/**
 * @brief   RTSP 세션의 connect/setup/play/run 생명주기를 추상화한 인터페이스
 *
 * @details
 * IMetadataSource는 Pipeline이 보는 pull 인터페이스이고,
 * INetwork는 RTSP 프로토콜 자체(Digest 인증, TCP 인터리브 프레이밍)를 감춘 push 인터페이스
 *
 * @note [ 일반화 ]
 * 현재 RTSP에 너무 치중됨
 * -- MQTT나 다른 프로토콜로 수정되었을 때 갈아엎어야 할 상황이 발생 가능성 존재
 *
 * @todo
 * - 프로토콜과 상관 없도록 추상화하기
 */
class INetwork {
public:
    virtual ~INetwork() = default;

    /**
     * @brief   CCTV와 TCP 연결을 수립
     * @return  성공 시 true
     */
    virtual bool connect() = 0;

    /**
     * @brief   RTSP SETUP(Digest 인증 포함)으로 스트리밍 세션을 수립
     * @return  성공 시 true
     */
    virtual bool setup() = 0;

    /**
     * @brief   RTSP PLAY 로 스트리밍을 시작하고 keepalive 를 구동
     */
    virtual void play() = 0;

    /**
     * @brief   인터리브 스트림을 블로킹으로 수신하는 루프
     * @details 연결이 끊기거나 오류가 나면 리턴 (재연결은 하지 않음)
     */
    virtual void run() = 0;

    /**
     * @brief   Metadata payload 하나가 재조합될 때마다 호출되는 콜백
     * @details
     * run()을 부르는 스레드에서 동기적으로 호출
     * 콜백 내부에서 오래 걸리는 작업을 하면 다음 RTP 패킷 수신이 지연됨
     */
    std::function<void(std::string_view)> onPayloadReceived;
};