#pragma once

/**
 * @file    RtspClientV2.h
 * @brief   RTSP Digest 인증 및 TCP 인터리브 방식 (저지연/저할당 최적화 버전)
 *
 * @details
 * 원본 RtspClient와 동일한 프로토콜(SETUP/PLAY/GET_PARAMETER, Digest 인증, TCP 인터리브)을
 * 구현하지만, 다음 3가지를 최적화함:
 *  1) 사용자 공간 버퍼링 recv() -> 패킷당 syscall 수를 크게 줄임
 *  2) TCP_NODELAY / SO_RCVBUF 설정으로 커널 레벨 지연 요소 제거
 *  3) 루프(run())에서 매 패킷마다 발생하던 vector::resize(재할당 + zero-init)를 제거하고
 *     고정 크기 버퍼 재사용으로 전환
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/INetwork.h"

/**
 * @class   RtspClientV2
 * @brief   RTSP Digest 인증(RFC 2617) + TCP 인터리브 방식 (최적화 버전)
 *
 * @details
 * 단일 연결 시도의 생명주기(connect -> setup -> play -> run)만 책임
 * 재연결/백오프 정책은 상위(Source)의 몫
 * -- run() 은 연결이 끊기면 그냥 리턴하고, 이 클래스는 스스로 재시도하지 않음
 */
class RtspClientV2 : public INetwork {
public:
    explicit RtspClientV2(const AppConfig& config);

    ~RtspClientV2() override;

    bool connect() override;
    bool setup() override;
    void play() override;
    void run() override;

private:
    /**
     * @brief   소켓에서 사용자 공간 버퍼(sockBuf_)를 한 번 채움 (recv() syscall 1회)
     * @return  성공 시 true, 연결 종료/오류 시 false
     */
    bool fillReadBuffer();

    /**
     * @brief   버퍼링된 스트림에서 1바이트를 읽음 (버퍼가 비면 fillReadBuffer 호출)
     */
    bool readByte(std::uint8_t& out);

    /**
     * @brief   버퍼링된 스트림에서 정확히 len 바이트를 dest에 읽어옴
     * @details 원본의 readExact와 달리 vector::resize 없이 미리 확보된 버퍼에 씀
     */
    bool readBytes(std::uint8_t* dest, std::size_t len);

    /**
     * @brief   소켓에서 헤더 종료("\r\n\r\n")까지 누적해서 읽음 (SETUP/PLAY 등 저빈도 경로)
     */
    bool recvHeaders(std::string& out);

    /**
     * @brief   RFC 2617 Digest 인증 Authorization 헤더 문자열을 생성
     */
    std::string buildDigestHeader(const std::string& method, const std::string& uri);

    /**
     * @brief   입력 문자열의 MD5 해시를 16진수 문자열로 계산
     */
    std::string md5Hex(const std::string& input);

    /**
     * @brief   30초 주기로 GET_PARAMETER를 보내 RTSP 세션을 유지하는 루프
     */
    void keepAliveLoop();

    /**
     * @brief   일정 주기(kMetricsReportIntervalMs)마다 누적된 성능 지표를 로그로 출력
     * @details 평균 프레임 조립 시간, 프레임당 recv() 호출 수, 처리율(fps/KB/s)을 보여줌
     *          -> 원본(RtspClient) 대비 개선 정도를 수치로 비교하기 위함
     */
    void reportMetricsIfDue();

    /// @name 프로토콜 불변 상수
    /// @details RTP/인터리브 프레이밍이 정한 값이라 설정으로 노출하지 않음
    ///          (튜닝 대상이 아니고, 바꾸면 파싱이 깨짐)
    /// @{

    static constexpr int kRtpHeaderSize = 12;

    /// @brief RTP 인터리브 프레임의 2바이트 length 필드가 표현 가능한 최댓값
    static constexpr std::size_t kMaxRtpPayloadSize = 65535;

    /// @}

    /**
     * @name 설정으로 빠진 튜닝 값들 (AppConfig)
     * @details
     * 예전엔 static constexpr 로 여기 박혀 있었음. 기본값은 performance/compute-server.md 에
     * 측정된 값 그대로이므로, config 에서 건드리지 않으면 문서의 지표가 그대로 유효함
     *
     * - readBufBytes_        : 이 크기 단위로 recv() 호출 -> 프레임당 syscall 수를 좌우 (측정값 65536)
     * - maxMetadataFrameSize_: marker 누락/스트림 손상 시 무한정 쌓이는 것을 막는 상한 (측정값 1MiB)
     *                          넘으면 run()이 버퍼를 버리고 종료해 상위(Source)가 재연결
     * - connectTimeoutSec_   : SO_RCVTIMEO 는 connect() 에 적용되지 않음. 논블로킹 connect +
     *                          select() 로 명시적 제한을 걸지 않으면 카메라 무응답/방화벽 SYN drop 시
     *                          커널 기본 타임아웃(1~2분)까지 아무 로그 없이 블로킹됨
     * @{
     */
    std::size_t readBufBytes_;
    std::size_t maxMetadataFrameSize_;
    int connectTimeoutSec_;
    int recvTimeoutSec_;
    int socketRecvBufBytes_;
    int keepAliveIntervalSec_;
    std::chrono::milliseconds metricsReportInterval_;
    /** @} */

    AppConfig cfg_;
    int sock_ = -1;
    std::string realm_;
    std::string nonce_;
    std::string sessionId_;
    int cseq_ = 4;
    int nonceCount_ = 1;
    std::atomic<bool> keepRunning_{false};
    std::thread keepaliveThread_;

    /// @name 버퍼링 recv() 상태 (동적 할당은 연결당 1회 reserve 뿐)
    /// @{
    std::vector<std::uint8_t> sockBuf_;
    std::size_t sockBufLen_ = 0;  ///< sockBuf_ 내 유효 데이터 길이
    std::size_t sockBufPos_ = 0;  ///< sockBuf_ 내 다음에 읽을 위치
    /// @}

    /// @brief RTP payload 재조합용 고정 버퍼 (매 패킷마다 resize 하지 않음)
    std::vector<std::uint8_t> rtpPacket_;

    /// @brief 성능 지표 누적 상태
    struct Metrics {
        std::uint64_t payloadCount = 0;                 ///< 조립 완료된 metadata payload 개수
        std::uint64_t recvSyscalls = 0;                 ///< 실제 recv() 호출 횟수
        std::uint64_t totalBytes = 0;                   ///< 조립된 payload 총 바이트 수
        std::chrono::nanoseconds totalAssembleTime{0};  ///< payload 조립에 걸린 시간 합
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    } metrics_;

    /// @brief 현재 조립 중인 metadata frame의 시작 시각 (지표 측정용)
    std::chrono::steady_clock::time_point frameAssembleStart_;
};
