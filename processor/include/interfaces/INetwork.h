/**
 * @file    INetwork.h
 * @brief   네트워크로부터 메타데이터 패킷을 수신하는 인터페이스
 */
#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "core/AppConfig.h"

/**
 * @brief   CCTV와의 네트워크 연결 및 스트림 수신을 담당하는 인터페이스
 * @details RTSP 외 다른 프로토콜로 교체될 가능성에 대비하여 인터페이스로 분리
 *          connect() → setup() → play() → run() 순서로 호출되는 것을 전제
 */
class INetwork {
public:
    virtual ~INetwork() = default;

    /**
     * @brief   카메라와의 소켓 연결을 수립
     * @return  연결 성공 시 true, 실패 시 false
     */
    virtual bool connect() = 0;

    /**
     * @brief   스트림 수신을 위한 세션 설정 (인증 등)
     * @return  설정 성공 시 true, 실패 시 false
     */
    virtual bool setup() = 0;

    /**
     * @brief   스트림 재생을 요청하여 데이터 수신을 시작
     * @note    connect(), setup()이 선행되어야 함
     */
    virtual void play() = 0;

    /**
     * @brief   스트림 데이터를 지속적으로 수신하는 루프
     * @note    호출 시 블로킹되며, 패킷 수신 시마다 onPayloadReceived 콜백을 호출
     */
    virtual void run() = 0;

    /**
     * @brief   패킷 수신 시 호출되는 콜백
     * @details 수신한 원본 패킷을 전달 (사용 전 반드시 등록해야 함)
     */
    std::function<void(std::string_view)> onPayloadReceived;
};

/**
 * @brief   INetwork 구현체를 생성하는 팩토리 함수
 * @param   config  CCTV 연결에 필요한 설정
 * @return  생성된 INetwork 구현체에 대한 shared_ptr
 */
std::shared_ptr<INetwork> createNetwork(const AppConfig& config);