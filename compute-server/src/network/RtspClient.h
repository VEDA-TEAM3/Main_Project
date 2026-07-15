#pragma once

/**
 * @file    RtspClient.h
 * @brief   RTSP Digest 인증 및 TCP 인터리브 방식으로 카메라와 통신하는 INetwork 구현체.
 */

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/INetwork.h"

/**
 * @class   RtspClient
 * @brief   RTSP Digest 인증(RFC 2617) + TCP 인터리브 방식 INetwork 구현체.
 *
 * @details
 * 단일 연결 시도의 생명주기(connect -> setup -> play -> run)만 책임.
 * 재연결/백오프 정책은 상위(Source)의 몫
 * -- run() 은 연결이 끊기면 그냥 리턴하고, 이 클래스는 스스로 재시도하지 않음.
 */
class RtspClient : public INetwork {
public:
    /**
     * @brief   RtspClient 생성자
     * @param   config  카메라 접속 정보 (AppConfig::rtsp* 필드 사용)
     */
    explicit RtspClient(const AppConfig& config);

    /**
     * @brief   소멸자. keepalive 스레드를 정지시키고 소켓을 닫는다.
     */
    ~RtspClient() override;

    bool connect() override;
    bool setup() override;
    void play() override;
    void run() override;

private:
    /**
     * @brief   소켓에서 헤더 종료("\r\n\r\n")까지 누적해서 읽음.
     * @param   out  누적된 응답 문자열 (출력)
     * @return  성공 시 true, 연결 종료/오류/상한 초과 시 false
     */
    bool recvHeaders(std::string& out);

    /**
     * @brief   RFC 2617 Digest 인증 Authorization 헤더 문자열을 생성.
     * @param   method  RTSP 메서드
     * @param   uri     요청 대상 URI
     * @return  "Authorization: Digest ..." 형식의 완성된 헤더 문자열
     */
    std::string buildDigestHeader(const std::string& method, const std::string& uri);

    /**
     * @brief   입력 문자열의 MD5 해시를 16진수 문자열로 계산.
     * @param   input  해시할 입력 문자열
     * @return  32자리 소문자 16진수 MD5 해시 문자열
     */
    std::string md5Hex(const std::string& input);

    /**
     * @brief   소켓에서 정확히 len 바이트를 읽어 buf 에 채움.
     * @param   buf  읽은 데이터를 저장할 버퍼
     * @param   len  읽어야 할 바이트 수
     * @return  len 바이트를 모두 읽으면 true, 중간에 연결이 끊기면 false
     */
    bool readExact(std::vector<std::uint8_t>& buf, std::size_t len);

    /**
     * @brief   30초 주기로 GET_PARAMETER 를 보내 RTSP 세션을 유지하는 루프.
     * @details 별도 스레드(keepaliveThread_)에서 실행.
     */
    void keepAliveLoop();

    static constexpr int kRtpHeaderSize = 12;

    AppConfig cfg_;
    int sock_ = -1;
    std::string realm_;
    std::string nonce_;
    std::string sessionId_;
    int cseq_ = 4;
    int nonceCount_ = 1;  ///< Digest nc. buildDigestHeader() 호출마다 증가.

    std::atomic<bool> keepRunning_{false};
    std::thread keepaliveThread_;
};