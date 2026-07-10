/**
 * @file    Processor.h
 * @brief   Network → Parser → Trans → Sender로 이어지는 처리 파이프라인
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "interfaces/INetwork.h"
#include "interfaces/IParser.h"
#include "interfaces/ISender.h"
#include "interfaces/ITrans.h"

/**
 * @brief   영상 스트림에서 메타데이터를 수신하여 관제 서버로 전송하기까지의
 *          전체 처리 흐름을 담당하는 클래스
 * @details Network로부터 원본 패킷을 수신하고, Parser로 ParsedFrame
 *          (프레임 타임스탬프 + 탐지 객체 목록)을 추출한 뒤, 프레임 내 각 탐지
 *          객체를 Trans로 Top-view 좌표로 변환하고, buildPacket()으로 프레임
 *          단위 패킷 하나를 구성하여 Sender로 전송한다.
 */
class Processor {
public:
    /**
     * @brief   Processor 생성자
     * @param   network Network 인터페이스 구현체 (패킷 수신 담당)
     * @param   parser  Parser 인터페이스 구현체 (패킷 파싱 담당)
     * @param   trans   Trans 인터페이스 구현체 (좌표 변환 담당)
     * @param   sender  Sender 인터페이스 구현체 (관제 서버 전송 담당)
     */
    Processor(std::shared_ptr<INetwork> network, std::shared_ptr<IParser> parser, std::shared_ptr<ITrans> trans,
              std::shared_ptr<ISender> sender);

    /**
     * @brief   Network 연결부터 스트림 수신 루프까지 파이프라인을 실행
     * @return  정상 종료 시 true, 초기화 실패 시 false
     * @note    내부에서 network_->run()까지 호출하므로 블로킹 호출
     */
    bool run();

private:
    /**
     * @brief   Network로부터 원본 패킷을 수신했을 때 호출되는 콜백
     * @param   Network 계층에서 수신한 원본 바이트 데이터
     * @details 내부적으로 parser_->parse(raw)로 ParsedFrame을 얻고, objects가
     *          비어있으면(탐지된 객체 없음) 아무 것도 전송하지 않음
     *          객체가 하나 이상 있으면 각 객체를 trans_->transform()으로 Top-view
     *          좌표로 변환한 뒤, buildPacket()으로 프레임 전체를 하나의 패킷으로
     *          묶어 sender_->send()로 전송
     */
    void onPayload(std::string_view raw);

    /**
     * @brief   프레임 타임스탬프, 탐지 객체 목록, 변환된 좌표 목록으로
     *          관제 서버 전송용 패킷 하나를 생성
     * @param   timestamp_ms    프레임 타임스탬프
     * @param   objects         프레임 내 탐지된 객체 목록
     * @param   worldPoints     각 객체에 대응하는 Top-view 좌표 목록
     * @return  관제 서버로 전송할 패킷 문자열
     * @note    objects와 worldPoints는 크기와 순서가 반드시 일치해야 함
     */
    std::string buildPacket(int64_t timestamp_ms, const std::vector<DetectedObject>& objects,
                            const std::vector<WorldPoint>& worldPoints);

    std::shared_ptr<INetwork> network_; /**< 패킷 수신용 Network 구현체 */
    std::shared_ptr<IParser> parser_;   /**< 패킷 파싱용 Parser 구현체 */
    std::shared_ptr<ITrans> trans_;     /**< 좌표 변환용 Trans 구현체 */
    std::shared_ptr<ISender> sender_;   /**< 관제 서버 전송용 Sender 구현체 */
};