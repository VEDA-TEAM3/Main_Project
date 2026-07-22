#include "network/RtspClientV2.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/md5.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Network";

}  // namespace

RtspClientV2::RtspClientV2(const AppConfig& config) : cfg_(config) {
    sockBuf_.resize(kSocketReadBufSize);
    rtpPacket_.resize(kMaxRtpPayloadSize);
}

RtspClientV2::~RtspClientV2() {
    keepRunning_ = false;
    if (keepaliveThread_.joinable())
        keepaliveThread_.join();
    if (sock_ != -1) {
        close(sock_);
        logSuccess(kIface, "소켓 정상 종료");
    }
}

bool RtspClientV2::connect() {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        logError(kIface, "소켓 생성 실패");
        return false;
    }

    struct timeval tv {
        5, 0
    };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const int noDelay = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    const int rcvBufBytes = 1 << 20;  // 1MB
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvBufBytes, sizeof(rcvBufBytes));

    struct sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(cfg_.rtspPort));
    inet_pton(AF_INET, cfg_.rtspIp.c_str(), &serverAddr.sin_addr);

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        logError(kIface, "연결 실패 (" + cfg_.rtspIp + ":" + std::to_string(cfg_.rtspPort) + ")");
        return false;
    }
    logSuccess(kIface, "연결 성공 (" + cfg_.rtspIp + ":" + std::to_string(cfg_.rtspPort) + ")");
    return true;
}

bool RtspClientV2::fillReadBuffer() {
    sockBufPos_ = 0;
    const ssize_t n = recv(sock_, sockBuf_.data(), sockBuf_.size(), 0);
    ++metrics_.recvSyscalls;
    if (n <= 0) {
        sockBufLen_ = 0;
        return false;
    }
    sockBufLen_ = static_cast<std::size_t>(n);
    return true;
}

bool RtspClientV2::readByte(std::uint8_t& out) {
    if (sockBufPos_ >= sockBufLen_) {
        if (!fillReadBuffer())
            return false;
    }
    out = sockBuf_[sockBufPos_++];
    return true;
}

bool RtspClientV2::readBytes(std::uint8_t* dest, std::size_t len) {
    std::size_t copied = 0;
    while (copied < len) {
        if (sockBufPos_ >= sockBufLen_) {
            if (!fillReadBuffer())
                return false;
        }
        const std::size_t avail = sockBufLen_ - sockBufPos_;
        const std::size_t take = std::min(avail, len - copied);
        std::memcpy(dest + copied, sockBuf_.data() + sockBufPos_, take);
        sockBufPos_ += take;
        copied += take;
    }
    return true;
}

bool RtspClientV2::recvHeaders(std::string& out) {
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        const int n = recv(sock_, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 65536)
            return false;
    }
    return true;
}

bool RtspClientV2::setup() {
    std::string req1 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 1\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
    send(sock_, req1.c_str(), req1.length(), 0);

    std::string res1;
    if (!recvHeaders(res1)) {
        logError(kIface, "SETUP 1차 응답 수신 실패");
        return false;
    }

    const size_t realmPos = res1.find("realm=\"");
    const size_t noncePos = res1.find("nonce=\"");
    if (realmPos != std::string::npos)
        realm_ = res1.substr(realmPos + 7, res1.find('"', realmPos + 7) - (realmPos + 7));
    if (noncePos != std::string::npos)
        nonce_ = res1.substr(noncePos + 7, res1.find('"', noncePos + 7) - (noncePos + 7));
    if (nonce_.empty()) {
        logError(kIface, "인증 파라미터(nonce) 파싱 실패");
        return false;
    }

    std::string auth = buildDigestHeader("SETUP", cfg_.rtspSetupUri);
    std::string req2 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 2\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" +
                       auth + "\r\n";
    send(sock_, req2.c_str(), req2.length(), 0);

    std::string res2;
    if (!recvHeaders(res2)) {
        logError(kIface, "SETUP 2차(인증) 응답 수신 실패");
        return false;
    }

    const size_t sessPos = res2.find("Session: ");
    if (sessPos == std::string::npos) {
        logError(kIface, "세션 ID 파싱 실패");
        return false;
    }

    sessionId_ = res2.substr(sessPos + 9, res2.find_first_of(";\r\n", sessPos) - (sessPos + 9));
    logSuccess(kIface, "인증 성공, 세션 ID: " + sessionId_);
    return true;
}

void RtspClientV2::play() {
    std::string auth = buildDigestHeader("PLAY", cfg_.rtspPlayUri);
    std::string req =
        "PLAY " + cfg_.rtspPlayUri + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + sessionId_ + "\r\n" + auth + "\r\n";
    send(sock_, req.c_str(), req.length(), 0);

    std::string res;
    recvHeaders(res);
    logSuccess(kIface, "PLAY 명령 전송 완료, 스트리밍 시작");

    keepRunning_ = true;
    keepaliveThread_ = std::thread(&RtspClientV2::keepAliveLoop, this);
}

void RtspClientV2::run() {
    std::string metadataBuffer;
    metadataBuffer.reserve(8192);

    while (true) {
        std::uint8_t sync = 0;
        if (!readByte(sync))
            break;
        if (sync != '$')
            continue;

        std::uint8_t header[3];
        if (!readBytes(header, 3))
            break;
        const int channel = header[0];
        const int payloadLen = (header[1] << 8) | header[2];

        if (static_cast<std::size_t>(payloadLen) > kMaxRtpPayloadSize)
            break;

        if (!readBytes(rtpPacket_.data(), static_cast<std::size_t>(payloadLen)))
            break;
        if (channel != 0)
            continue;
        if (payloadLen < kRtpHeaderSize)
            continue;

        if (metadataBuffer.empty())
            frameAssembleStart_ = std::chrono::steady_clock::now();

        const bool marker = (rtpPacket_[1] & 0x80) != 0;
        metadataBuffer.append(reinterpret_cast<const char*>(rtpPacket_.data() + kRtpHeaderSize),
                              static_cast<size_t>(payloadLen - kRtpHeaderSize));

        if (metadataBuffer.size() > kMaxMetadataFrameBytes) {
            // marker bit가 누락됐거나 스트림이 손상된 것으로 간주 -> 무한정 쌓이기 전에
            // 버퍼를 버리고 연결 자체를 재수립 (상위 Source의 재연결 백오프에 위임)
            logError(kIface, "metadata 프레임 크기가 상한(" + std::to_string(kMaxMetadataFrameBytes) +
                                  "B)을 초과 (marker 누락 의심) - 연결 재수립");
            break;
        }

        if (marker) {
            const auto elapsed = std::chrono::steady_clock::now() - frameAssembleStart_;
            metrics_.totalAssembleTime += elapsed;
            metrics_.totalBytes += metadataBuffer.size();
            ++metrics_.payloadCount;

            if (onPayloadReceived)
                onPayloadReceived(std::string_view(metadataBuffer));
            metadataBuffer.clear();

            reportMetricsIfDue();
        }
    }

    keepRunning_ = false;
    logError(kIface, "스트림 종료 또는 연결 끊김");
}

void RtspClientV2::reportMetricsIfDue() {
    using namespace std::chrono;

    const auto now = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(now - metrics_.windowStart);
    if (elapsed < kMetricsReportInterval || metrics_.payloadCount == 0)
        return;

    const double avgAssembleUs = duration_cast<duration<double, std::micro>>(metrics_.totalAssembleTime).count() /
                                 static_cast<double>(metrics_.payloadCount);
    const double fps = static_cast<double>(metrics_.payloadCount) * 1000.0 / static_cast<double>(elapsed.count());
    const double recvCallsPerFrame =
        static_cast<double>(metrics_.recvSyscalls) / static_cast<double>(metrics_.payloadCount);
    const double throughputKBs =
        (static_cast<double>(metrics_.totalBytes) / 1024.0) * 1000.0 / static_cast<double>(elapsed.count());

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << "최근 " << elapsed.count() << "ms 지표 - 프레임 "
        << metrics_.payloadCount << "개, 평균 조립시간 " << avgAssembleUs << "us, 처리율 " << fps
        << "fps, recv() 호출/프레임 " << recvCallsPerFrame << "회, 처리량 " << throughputKBs << "KB/s";
    logSuccess(kIface, oss.str());

    metrics_.payloadCount = 0;
    metrics_.recvSyscalls = 0;
    metrics_.totalBytes = 0;
    metrics_.totalAssembleTime = nanoseconds{0};
    metrics_.windowStart = now;
}

std::string RtspClientV2::md5Hex(const std::string& input) {
    unsigned char hash[MD5_DIGEST_LENGTH];
    // NOLINTNEXTLINE
    MD5(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    char output[33];
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        snprintf(output + i * 2, 3, "%02x", hash[i]);
    return std::string(output);
}

std::string RtspClientV2::buildDigestHeader(const std::string& method, const std::string& uri) {
    char ncBuf[9];
    snprintf(ncBuf, sizeof(ncBuf), "%08x", nonceCount_++);
    const std::string nc(ncBuf);
    const std::string cnonce = "0a4f113b";
    const std::string qop = "auth";

    const std::string ha1 = md5Hex(cfg_.rtspUser + ":" + realm_ + ":" + cfg_.rtspPass);
    const std::string ha2 = md5Hex(method + ":" + uri);
    const std::string response = md5Hex(ha1 + ":" + nonce_ + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);

    char header[1024];
    snprintf(header, sizeof(header),
             "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
             "uri=\"%s\", response=\"%s\", qop=%s, nc=%s, cnonce=\"%s\"\r\n",
             cfg_.rtspUser.c_str(), realm_.c_str(), nonce_.c_str(), uri.c_str(), response.c_str(), qop.c_str(),
             nc.c_str(), cnonce.c_str());
    return std::string(header);
}

void RtspClientV2::keepAliveLoop() {
    while (keepRunning_) {
        for (int i = 0; i < 30 && keepRunning_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!keepRunning_)
            break;

        std::string auth = buildDigestHeader("GET_PARAMETER", cfg_.rtspPlayUri);
        std::string req = "GET_PARAMETER " + cfg_.rtspPlayUri + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq_++) +
                          "\r\nSession: " + sessionId_ + "\r\n" + auth + "\r\n";

        if (send(sock_, req.c_str(), req.length(), MSG_NOSIGNAL) < 0) {
            logError(kIface, "Keep-alive 전송 실패 - 연결 끊김 추정");
            break;
        }
    }
}
