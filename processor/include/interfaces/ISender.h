/**
 * @file    ISender.h
 * @brief   완성된 패킷을 관제 서버로 전송하는 인터페이스
 */
#pragma once

#include <memory>
#include <string>

/**
 * @brief   관제 서버로 패킷을 전송하는 인터페이스
 * @details 전송 프로토콜이 변경되거나 추가될 가능성에 대비하여 인터페이스로 분리
 */
class ISender {
public:
    virtual ~ISender() = default;

    /**
     * @brief   패킷을 관제 서버로 전송
     * @param   packet  전송할 패킷 데이터
     * @return  전송 성공 시 true, 실패 시 false
     */
    virtual bool send(const std::string& packet) = 0;
};

/**
 * @brief   ISender 구현체를 생성하는 팩토리 함수
 * @return  생성된 ISender 구현체에 대한 shared_ptr
 */
std::shared_ptr<ISender> createSender();