/**
 * @file    INetwork.h
 * @brief   외부 시스템과의 통신을 추상화하는 인터페이스
 * @note    통신 프로토콜이 .so로 별도 개발되어 dlopen되는 것을 전제로 한다.
 *          이 인터페이스는 특정 프로토콜의 개념에 종속되지 않도록
 *          최소한의 연결/송수신 동작만 정의한다.
 * @note    channelKey는 프로토콜에 따라 의미가 다르게 해석된다.
 *          - MQTT 구현체: MQTT 토픽으로 해석
 *          - TCP/IP 구현체: 목적지 host:port 혹은 단일 커넥션이면 무시
 * @note    연결 설정(브로커 주소, 포트 등)은 이 인터페이스가 아니라 구현체가
 *          자체적으로 소유한다.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class INetwork {
public:
    virtual ~INetwork() = default;

    /**
     * @brief   통신 대상에 연결한다.
     * @return  연결 성공 여부
     */
    virtual bool connect() = 0;

    /**
     * @brief   수신을 시작한다. MQTT라면 토픽 구독, TCP라면 단순히 no-op이거나
     *          연결 확인 정도로 구현될 수 있다.
     * @param   channelKey   구현체별 해석 방식에 따른 채널 식별자
     * @return  성공 여부
     */
    virtual bool startReceiving(const std::string& channelKey) = 0;

    /**
     * @brief   지정된 대상으로 데이터를 전송한다.
     * @param   channelKey   구현체별 해석 방식에 따른 채널 식별자
     * @param   payload      전송할 바이트 데이터
     * @return  전송 성공 여부
     */
    virtual bool send(const std::string& channelKey, const std::vector<uint8_t>& payload) = 0;

    /**
     * @brief   데이터를 수신한다.
     * @param   outPayload   수신된 바이트 데이터가 채워짐
     * @return  수신 성공 여부
     */
    virtual bool receive(std::vector<uint8_t>& outPayload) = 0;
};