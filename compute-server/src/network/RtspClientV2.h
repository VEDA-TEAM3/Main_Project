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

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/INetwork.h"

struct ssl_ctx_st;
struct ssl_st;

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

    /**
     * @brief   다른 스레드에서 진행 중인 세션을 즉시 취소 (멱등, 스레드 안전)
     *
     * @details
     * cancelled_를 세우고 소켓에 shutdown(SHUT_RDWR)을 걸어 blocking recv()를 그 자리에서
     * 깨운다 -> run() 이 recv 타임아웃(recvTimeoutSec_, 기본 5초)을 기다리지 않고 즉시 빠져나오므로
     * 워커 join 이 지연되지 않는다. 취소 플래그만 있으면 '스트림이 살아있는 동안' recv 가 계속
     * 성공해 run() 이 영영 반환하지 않으므로, shutdown()이 반드시 함께 필요하다
     *
     * @warning close()가 아니라 shutdown()이어야 한다. fd의 소유와 close는 소유자 스레드
     *          (소멸자)의 몫이며, 다른 스레드가 close 하면 fd 번호 재사용 경합이 생긴다
     */
    void cancel() noexcept;

    /// @brief PLAY가 200 OK로 성공해 스트리밍이 시작되었는가 (run() 진입 여부 판단용)
    bool playSucceeded() const noexcept { return playOk_; }

private:
    /**
     * @brief   소켓을 닫고 sock_ 를 무효화 (멱등)
     * @details connect()의 모든 실패 경로에서 호출해 fd를 즉시 반납한다. 소멸자에만 의존하면
     *          같은 인스턴스로 connect()를 재시도하는 순간 이전 fd가 새어나감
     */
    void closeSocket() noexcept;

    /**  RTSP 요청 전체를 전송하며 TCP short write와 EINTR를 처리 */
    bool sendAll(std::string_view request, const char* operation);
    ssize_t recvSome(void* buffer, std::size_t length) noexcept;
    bool startTls();

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
     * @brief   effectiveKeepAliveSec_ 주기로 GET_PARAMETER를 보내 RTSP 세션을 유지하는 루프
     *
     * @warning 주기는 설정값이 아니라 '카메라가 통보한 세션 타임아웃'에서 유도된다.
     *          자세한 이유는 deriveKeepAliveInterval() 참고
     */
    void keepAliveLoop();

    /**
     * @brief   SETUP 응답의 Session 헤더에서 timeout= 을 읽어 keep-alive 주기를 정한다
     *
     * @details
     * RTSP Session 헤더는 `Session: <id>[;timeout=<초>]` 형식이고, timeout 은
     * **"이 시간 안에 요청이 없으면 세션을 끊겠다"는 카메라의 통보**다 (RFC 2326 §12.37).
     *
     * 예전에는 이 값을 통째로 버리고 rtspKeepAliveIntervalSec(기본 30초) 고정 주기로만
     * GET_PARAMETER 를 보냈다. 카메라가 `timeout=10` 을 통보하면 30초짜리 keep-alive 는
     * 영원히 늦어서, 스트림이 10초마다 끊기고 재연결되는 루프에 빠진다
     * -- PLAY 는 매번 성공하므로 인증/URI 문제처럼 보이지 않아 원인을 찾기 어렵다
     *
     * 통보된 타임아웃의 절반을 주기로 삼는다(최소 1초). 절반인 이유는 한 번 유실돼도
     * 다음 keep-alive 가 만료 전에 도달하기 때문이다. 설정값이 더 짧으면 설정값을 쓴다
     *
     * @param   sessionLine SETUP 응답의 Session 헤더 한 줄
     */
    void deriveKeepAliveInterval(const std::string& sessionLine);

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

    /**
     * @brief RTP 고정 헤더 길이 (V/P/X/CC, M/PT, seq, timestamp, SSRC)
     * @warning 이것은 '최소' 길이다. 실제 헤더는 CSRC 개수와 확장 헤더에 따라 더 길어진다
     *          -- rtpHeaderLength() 로 계산할 것 (고정 12로 자르면 페이로드 앞에 이진 쓰레기가 붙는다)
     */
    static constexpr int kRtpHeaderSize = 12;

    /// @brief RTP 버전 (RFC 3550). 첫 바이트 상위 2비트가 이 값이 아니면 RTP 패킷이 아니다
    static constexpr std::uint8_t kRtpVersion = 2;

    /**
     * @brief   실제 RTP 헤더 길이를 계산 (고정 12 + CSRC + 확장 헤더)
     * @param   packet    RTP 패킷 선두
     * @param   packetLen 패킷 전체 길이
     * @return  헤더 길이. RTP 가 아니거나 길이가 모순이면 0
     */
    static std::size_t rtpHeaderLength(const std::uint8_t* packet, std::size_t packetLen);

    /// @brief RTP 인터리브 프레임의 2바이트 length 필드가 표현 가능한 최댓값
    static constexpr std::size_t kMaxRtpPayloadSize = 65535;

    /// @}

    /**
     * @name 설정으로 빠진 튜닝 값들 (AppConfig)
     * @details
     * 예전엔 static constexpr 로 여기 박혀 있었음
     * 기본값은 performance/compute-server.md에
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

    /// @brief 카메라가 Session 헤더로 통보한 세션 타임아웃(초). 0 = 통보 없음
    int sessionTimeoutSec_ = 0;

    /// @brief 실제 keep-alive 주기 = min(설정값, 통보 타임아웃/2), 최소 1초
    int effectiveKeepAliveSec_ = 0;

    /**
     * @brief   메타데이터 RTP 가 실려오는 인터리브 채널 번호
     * @details SETUP 에서 `interleaved=0-1` 을 요청하지만, 서버가 다른 채널을 배정할 수 있다
     *          (RFC 2326 §12.39 -- 요청은 '희망'이고 응답이 확정이다).
     *          응답의 Transport 헤더를 파싱해 채워지며, 없으면 요청값 0 을 유지한다
     */
    int rtpChannel_ = 0;

    AppConfig cfg_;
    /** @brief fd 공개·shutdown·close를 직렬화해 fd 번호 재사용 경쟁을 방지 */
    mutable std::mutex socketMutex_;
    std::mutex tlsIoMutex_;
    int sock_ = -1;
    ssl_ctx_st* sslContext_ = nullptr;
    ssl_st* ssl_ = nullptr;
    /// @brief 취소 요청 여부 -- run() 루프가 매 반복 확인
    std::atomic<bool> cancelled_{false};
    /// @brief PLAY가 200 OK 였는가 (play()가 설정, workerLoop 가 run() 진입 판단에 사용)
    bool playOk_ = false;
    std::string realm_;
    std::string nonce_;
    std::string cnonce_;  ///< 세션마다 무작위로 생성하는 client nonce (Digest 재전송 공격 방어)
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
