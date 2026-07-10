/**
 * @file    RtspClient.cpp
 * @brief   RTSP Digest 인증 및 TCP 인터리브 방식으로 카메라와 통신하는 INetwork 구현체
 * @details RtspClient는 익명 네임스페이스 안에 숨겨져 있으며, 외부에는
 *          createNetwork() 팩토리 함수를 통해서만 INetwork로 노출
 */
#include <arpa/inet.h>
#include <openssl/md5.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/AppContext.h"
#include "interfaces/INetwork.h"

namespace {

/**
 * @brief   RTSP over TCP(interleaved) 방식으로 카메라에 접속하여
 *          ONVIF 메타데이터 스트림을 수신하는 INetwork 구현체
 * @details RTSP Digest 인증(RFC 2617)을 직접 구현하며, connect() → setup()
 *          → play() → run() 순서로 호출되는 것을 전제
 *          play() 이후 별도 스레드에서 30초 주기로 GET_PARAMETER 요청을 보내 세션을 유지
 *
 *          run()은 channel 0으로 수신되는 각 인터리브 payload를 RTP 패킷으로
 *          간주하여 12바이트 RTP 헤더를 제거하고, RTP 헤더의 marker bit를 기준으로
 *          여러 RTP 패킷에 걸쳐 조각난 메타데이터 XML을 재조합
 *          marker bit가 설정된 패킷을 만나면 그때까지 누적된 데이터를 완전한 하나의 payload로
 *          간주하여 onPayloadReceived를 호출
 */
class RtspClient : public INetwork {
public:
    /**
     * @brief   RtspClient 생성자
     * @param   config  카메라 접속에 필요한 설정 (ip, port, 인증 정보, URI 등)
     */
    explicit RtspClient(const AppConfig& config) : cfg_(config), sock_(-1), cseq_(4) {}

    /**
     * @brief   RtspClient 소멸자
     *          keepalive 스레드를 정지시키고 소켓을 닫음
     */
    ~RtspClient() override {
        keep_running_ = false;
        if (keepalive_thread_.joinable()) {
            keepalive_thread_.join();
        }
        if (sock_ != -1) {
            close(sock_);
            std::cout << "[*] Socket closed cleanly.\n";
        }
    }

    /**
     * @brief   카메라와 TCP 소켓 연결을 수립
     * @return  연결 성공 시 true, 소켓 생성 또는 connect() 실패 시 false
     */
    bool connect() override {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) {
            std::cerr << "[-] Socket creation failed\n";
            return false;
        }

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(cfg_.port);
        inet_pton(AF_INET, cfg_.ip.c_str(), &server_addr.sin_addr);

        if (::connect(sock_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "[-] Connect failed\n";
            return false;
        }
        std::cout << "[*] Connected to Camera (" << cfg_.ip << ":" << cfg_.port << ")\n";
        return true;
    }

    /**
     * @brief   RTSP SETUP 요청 2회(1차: nonce 획득, 2차: Digest 인증)를 통해
     *          스트리밍 세션을 수립
     * @return  세션 수립 성공 시 true, 인증 실패 또는 응답 파싱 실패 시 false
     * @note    1차 SETUP은 인증 없이 보내 401 응답에서 realm/nonce를 얻고,
     *          2차 SETUP에서 Digest Authorization 헤더를 포함해 재요청
     */
    bool setup() override {
        char buffer[4096];

        std::string req1 = "SETUP " + cfg_.setup_uri +
                           " RTSP/1.0\r\n"
                           "CSeq: 1\r\n"
                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
        send(sock_, req1.c_str(), req1.length(), 0);

        int n = recv(sock_, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0)
            return false;

        std::string res1(buffer, n);
        size_t realm_pos = res1.find("realm=\"");
        size_t nonce_pos = res1.find("nonce=\"");

        if (realm_pos != std::string::npos)
            realm_ = res1.substr(realm_pos + 7, res1.find("\"", realm_pos + 7) - (realm_pos + 7));
        if (nonce_pos != std::string::npos)
            nonce_ = res1.substr(nonce_pos + 7, res1.find("\"", nonce_pos + 7) - (nonce_pos + 7));
        if (nonce_.empty())
            return false;

        std::string auth = buildDigestHeader("SETUP", cfg_.setup_uri);
        std::string req2 = "SETUP " + cfg_.setup_uri +
                           " RTSP/1.0\r\n"
                           "CSeq: 2\r\n"
                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" +
                           auth + "\r\n";
        send(sock_, req2.c_str(), req2.length(), 0);

        memset(buffer, 0, sizeof(buffer));
        n = recv(sock_, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0)
            return false;

        std::string res2(buffer, n);
        size_t sess_pos = res2.find("Session: ");
        if (sess_pos != std::string::npos) {
            session_id_ = res2.substr(sess_pos + 9, res2.find_first_of(";\r\n", sess_pos) - (sess_pos + 9));
            std::cout << "[*] Successfully Auth! Session ID: " << session_id_ << "\n";
            return true;
        }
        return false;
    }

    /**
     * @brief   RTSP PLAY 요청을 보내 스트리밍을 시작하고, keepalive 스레드를 구동
     * @note    setup()이 선행되어 session_id_가 설정되어 있어야 함
     *          내부적으로 30초 주기 GET_PARAMETER를 보내는 keepAliveLoop()을
     *          별도 스레드로 시작
     */
    void play() override {
        std::string auth = buildDigestHeader("PLAY", cfg_.rtsp_uri);
        std::string req = "PLAY " + cfg_.rtsp_uri +
                          " RTSP/1.0\r\n"
                          "CSeq: 3\r\n"
                          "Session: " +
                          session_id_ + "\r\n" + auth + "\r\n";
        send(sock_, req.c_str(), req.length(), 0);

        char buffer[1024];
        recv(sock_, buffer, sizeof(buffer) - 1, 0);
        std::cout << "[*] PLAY command sent. Stream starting...\n";

        keep_running_ = true;
        keepalive_thread_ = std::thread(&RtspClient::keepAliveLoop, this);
    }

    /**
     * @brief   RTSP 인터리브(TCP) 스트림을 블로킹 방식으로 수신하며,
     *          RTP 패킷을 재조합하여 완전한 메타데이터 payload를 전달하는 루프
     *
     * @details '$' 마커로 시작하는 인터리브 프레임(1바이트 채널 + 2바이트 길이)을
     *          파싱하여 RTP 패킷을 읽음
     *
     *          channel 0(메타데이터 채널)일 경우 RTP 헤더(12바이트)를 제거한 나머지를
     *          metadata_buffer_에 누적하고,
     *          RTP 헤더의 marker bit가 설정된 패킷을 만나면 그때까지 누적된
     *          데이터를 완전한 payload로 간주하여
     *          onPayloadReceived를 호출하고 버퍼를 비움
     *
     *          소켓이 끊기거나 읽기 실패 시 루프를 종료
     */
    void run() override {
        std::vector<uint8_t> header;
        std::vector<uint8_t> rtp_packet;
        std::string metadata_buffer;

        while (true) {
            if (!readExact(header, 1))
                break;

            if (header[0] != '$')
                continue;

            if (!readExact(header, 3))
                break;
            int channel = header[0];
            int payload_len = (header[1] << 8) | header[2];

            if (!readExact(rtp_packet, payload_len))
                break;

            if (channel != 0)
                continue;

            if (payload_len < kRtpHeaderSize)
                continue;

            bool marker = (rtp_packet[1] & 0x80) != 0;

            metadata_buffer.append(reinterpret_cast<const char*>(rtp_packet.data() + kRtpHeaderSize),
                                   payload_len - kRtpHeaderSize);

            if (marker) {
                if (onPayloadReceived) {
                    onPayloadReceived(std::string_view(metadata_buffer));
                }
                metadata_buffer.clear();
            }
        }

        keep_running_ = false;
        std::cout << "[-] Stream ended or connection lost.\n";
    }

private:
    /** @brief  고정 RTP 헤더 크기 */
    static constexpr int kRtpHeaderSize = 12;

    /**
     * @brief   입력 문자열의 MD5 해시를 16진수 문자열로 계산
     * @param   input   해시할 입력 문자열
     * @return  32자리 소문자 16진수 MD5 해시 문자열
     */
    std::string md5_hex(const std::string& input) {
        unsigned char hash[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
        char output[33];
        for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
            snprintf(output + (i * 2), 3, "%02x", hash[i]);
        }
        return std::string(output);
    }

    /**
     * @brief   RFC 2617 Digest 인증 Authorization 헤더 문자열을 생성
     * @param   method  RTSP 메서드
     * @param   uri     요청 대상 URI
     * @return  "Authorization: Digest ..." 형식의 완성된 헤더 문자열
     */
    std::string buildDigestHeader(const std::string& method, const std::string& uri) {
        const std::string nc = "00000001", cnonce = "0a4f113b", qop = "auth";
        std::string ha1 = md5_hex(cfg_.user + ":" + realm_ + ":" + cfg_.pass);
        std::string ha2 = md5_hex(method + ":" + uri);
        std::string response = md5_hex(ha1 + ":" + nonce_ + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);

        char header[1024];
        snprintf(header, sizeof(header),
                 "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                 "uri=\"%s\", response=\"%s\", qop=%s, nc=%s, cnonce=\"%s\"\r\n",
                 cfg_.user.c_str(), realm_.c_str(), nonce_.c_str(), uri.c_str(), response.c_str(), qop.c_str(),
                 nc.c_str(), cnonce.c_str());
        return std::string(header);
    }

    /**
     * @brief   소켓에서 정확히 len 바이트를 읽어 buf에 채움
     * @param   buf 읽은 데이터를 저장할 버퍼
     * @param   len 읽어야 할 바이트 수
     * @return  len 바이트를 모두 읽으면 true, 중간에 연결이 끊기거나 오류 시 false
     */
    bool readExact(std::vector<uint8_t>& buf, size_t len) {
        buf.resize(len);
        size_t total_read = 0;
        while (total_read < len) {
            ssize_t n = recv(sock_, buf.data() + total_read, len - total_read, 0);
            if (n <= 0)
                return false;
            total_read += n;
        }
        return true;
    }

    /**
     * @brief   30초 주기로 GET_PARAMETER 요청을 보내 RTSP 세션을 유지하는 루프
     * @note    별도 스레드(keepalive_thread_)에서 실행되며, keep_running_이
     *          false가 되거나 전송 실패 시 루프를 종료
     */
    void keepAliveLoop() {
        while (keep_running_) {
            for (int i = 0; i < 30 && keep_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!keep_running_)
                break;

            std::string auth = buildDigestHeader("GET_PARAMETER", cfg_.rtsp_uri);
            std::string req = "GET_PARAMETER " + cfg_.rtsp_uri +
                              " RTSP/1.0\r\n"
                              "CSeq: " +
                              std::to_string(cseq_++) +
                              "\r\n"
                              "Session: " +
                              session_id_ + "\r\n" + auth + "\r\n";

            if (send(sock_, req.c_str(), req.length(), MSG_NOSIGNAL) < 0) {
                std::cerr << "[-] Keep-alive send failed. Connection likely lost.\n";
                break;
            }
        }
    }

    AppConfig cfg_;          /**< 카메라 접속 설정 */
    int sock_;               /**< 카메라와의 TCP 소켓 파일 디스크립터 */
    std::string realm_;      /**< Digest 인증 realm (SETUP 401 응답에서 획득) */
    std::string nonce_;      /**< Digest 인증 nonce (SETUP 401 응답에서 획득) */
    std::string session_id_; /**< RTSP 세션 ID (SETUP 200 응답에서 획득) */
    int cseq_;               /**< keepalive 요청에 사용되는 CSeq 카운터 */

    std::atomic<bool> keep_running_{false}; /**< keepalive/run 루프 지속 여부 플래그 */
    std::thread keepalive_thread_;          /**< keepAliveLoop()을 실행하는 스레드 */
};

}  // namespace

std::shared_ptr<INetwork> createNetwork(const AppConfig& config) { return std::make_shared<RtspClient>(config); }