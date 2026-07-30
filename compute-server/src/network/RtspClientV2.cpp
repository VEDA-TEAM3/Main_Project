#include "network/RtspClientV2.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/md5.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Network";

/// @brief RTSP 응답의 첫 줄(상태 줄, 예: "RTSP/1.0 401 Unauthorized")만 잘라냄 - 진단용
std::string statusLine(const std::string& response) {
    const size_t end = response.find("\r\n");
    return end == std::string::npos ? response : response.substr(0, end);
}

}  // namespace

RtspClientV2::RtspClientV2(const AppConfig& config)
    : readBufBytes_(static_cast<std::size_t>(config.rtspReadBufBytes)),
      maxMetadataFrameSize_(static_cast<std::size_t>(config.rtspMaxMetadataFrameBytes)),
      connectTimeoutSec_(config.rtspConnectTimeoutSec),
      recvTimeoutSec_(config.rtspRecvTimeoutSec),
      socketRecvBufBytes_(config.rtspSocketRecvBufBytes),
      keepAliveIntervalSec_(config.rtspKeepAliveIntervalSec),
      metricsReportInterval_(config.metricsReportIntervalMs),
      cfg_(config) {
    sockBuf_.resize(readBufBytes_);
    rtpPacket_.resize(kMaxRtpPayloadSize);

    // client nonce 를 세션마다 무작위로 생성 (하드코딩된 고정 cnonce는 Digest 재전송 공격에 취약)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dist;
    char cnonceBuf[9];
    snprintf(cnonceBuf, sizeof(cnonceBuf), "%08x", dist(gen));
    cnonce_ = cnonceBuf;
}

RtspClientV2::~RtspClientV2() {
    keepRunning_ = false;
    if (keepaliveThread_.joinable()) {
        keepaliveThread_.join();
    }
    const bool wasOpen = (sock_ != -1);
    closeSocket();
    if (wasOpen) {
        logSuccess(kIface, "소켓 정상 종료");
    }
}

void RtspClientV2::closeSocket() noexcept {
    // close 전에 무효화: 이후 cancel()이 들어와도 이미 닫힌(또는 번호가 재사용된) fd에 shutdown 하지 않음
    cancelFd_.store(-1, std::memory_order_release);
    if (sock_ != -1) {
        close(sock_);
        sock_ = -1;
    }
}

void RtspClientV2::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);

    // shutdown() 은 fd를 닫지 않고 연결만 끊으므로, 블로킹 중인 recv()가 즉시 0/-1로 반환된다.
    // (close()를 쓰면 소유자 스레드가 같은 fd 번호를 다시 쓰는 순간 경합이 생김)
    const int fd = cancelFd_.load(std::memory_order_acquire);
    if (fd != -1) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

bool RtspClientV2::connect() {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        sock_ = -1;  // 불변식 유지: 실패 시 항상 -1
        logError(kIface, "소켓 생성 실패");
        return false;
    }

    // cancel() 이 다른 스레드에서 이 fd 에 shutdown() 을 걸 수 있도록 공개 (원자적)
    cancelFd_.store(sock_, std::memory_order_release);

    struct timeval tv {
        recvTimeoutSec_, 0
    };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const int noDelay = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &socketRecvBufBytes_, sizeof(socketRecvBufBytes_));

    struct sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(cfg_.rtspPort));
    // inet_pton 은 점 표기 IPv4 만 처리한다. 실패(호스트명/오타)를 확인하지 않으면 sin_addr 가
    // 0 인 채로 0.0.0.0 에 접속을 시도해 원인 모를 실패로 이어진다
    if (inet_pton(AF_INET, cfg_.rtspIp.c_str(), &serverAddr.sin_addr) != 1) {
        logError(kIface, "잘못된 rtspIp=\"" + cfg_.rtspIp + "\" - 점 표기 IPv4 주소여야 합니다 (호스트명 미지원)");
        closeSocket();
        return false;
    }

    // connect() 자체는 SO_RCVTIMEO의 영향을 받지 않으므로, 논블로킹으로 전환한 뒤
    // select()로 명시적 타임아웃을 건다 (카메라 무응답/방화벽 SYN drop 시 무한 대기 방지)
    const int origFlags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, origFlags | O_NONBLOCK);

    const int connectResult = ::connect(sock_, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (connectResult < 0 && errno != EINPROGRESS) {
        logError(kIface,
                 "연결 실패 (" + cfg_.rtspIp + ":" + std::to_string(cfg_.rtspPort) + ") - " + std::strerror(errno));
        closeSocket();  // 소멸자에만 맡기지 않고 즉시 반납 (같은 인스턴스 재시도 시 fd 누수 방지)
        return false;
    }

    if (connectResult != 0) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock_, &writeSet);
        struct timeval connectTimeout {
            connectTimeoutSec_, 0
        };

        const int selectResult = select(sock_ + 1, nullptr, &writeSet, nullptr, &connectTimeout);
        if (selectResult <= 0) {
            logError(kIface, "연결 시도 타임아웃 (" + cfg_.rtspIp + ":" + std::to_string(cfg_.rtspPort) + ", " +
                                 std::to_string(connectTimeoutSec_) + "초)");
            closeSocket();
            return false;
        }

        int sockErr = 0;
        socklen_t sockErrLen = sizeof(sockErr);
        getsockopt(sock_, SOL_SOCKET, SO_ERROR, &sockErr, &sockErrLen);
        if (sockErr != 0) {
            logError(kIface, "연결 실패 (" + cfg_.rtspIp + ":" + std::to_string(cfg_.rtspPort) + ") - " +
                                 std::strerror(sockErr));
            closeSocket();
            return false;
        }
    }

    // 이후 recvHeaders/readBytes 등은 블로킹 소켓을 전제로 하므로 원래 모드로 복원
    fcntl(sock_, F_SETFL, origFlags);

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
        if (n <= 0) {
            return false;
        }

        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 65536) {  // 하드 코딩..?
            return false;
        }
    }
    return true;
}

bool RtspClientV2::setup() {
    std::string req1 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 1\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
    // MSG_NOSIGNAL: 카메라가 이미 연결을 끊었으면 send() 가 SIGPIPE 를 내며 프로세스를 죽일 수 있음
    if (send(sock_, req1.c_str(), req1.length(), MSG_NOSIGNAL) < 0) {
        logError(kIface, std::string("SETUP 1차 요청 전송 실패 - ") + std::strerror(errno));
        return false;
    }

    std::string res1;
    if (!recvHeaders(res1)) {
        logError(kIface, "SETUP 1차 응답 수신 실패");
        return false;
    }

    const size_t realmPos = res1.find("realm=\"");
    const size_t noncePos = res1.find("nonce=\"");
    if (realmPos != std::string::npos) {
        realm_ = res1.substr(realmPos + 7, res1.find('"', realmPos + 7) - (realmPos + 7));
    }
    if (noncePos != std::string::npos) {
        nonce_ = res1.substr(noncePos + 7, res1.find('"', noncePos + 7) - (noncePos + 7));
    }
    if (nonce_.empty()) {
        logError(kIface, "인증 파라미터(nonce) 파싱 실패 - SETUP 1차 응답: [" + statusLine(res1) + "]");
        return false;
    }

    std::string auth = buildDigestHeader("SETUP", cfg_.rtspSetupUri);
    std::string req2 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 2\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" +
                       auth + "\r\n";
    if (send(sock_, req2.c_str(), req2.length(), MSG_NOSIGNAL) < 0) {
        logError(kIface, std::string("SETUP 2차(인증) 요청 전송 실패 - ") + std::strerror(errno));
        return false;
    }

    std::string res2;
    if (!recvHeaders(res2)) {
        logError(kIface, "SETUP 2차(인증) 응답 수신 실패");
        return false;
    }

    const size_t sessPos = res2.find("Session: ");
    if (sessPos == std::string::npos) {
        // Session 헤더가 없다 = 카메라가 인증 SETUP을 200 OK로 받지 않았다는 뜻
        // 상태 줄로 사유를 구분: 401=인증 거부(계정/비밀번호), 4xx=URI/Transport 문제
        // 이 클라이언트는 DESCRIBE 없이 곧바로 SETUP하므로 rtspSetupUri가 정확해야 함
        logError(kIface, "세션 ID 파싱 실패 - SETUP 2차 응답: [" + statusLine(res2) +
                             "] (401=인증 거부, 454/455/461=URI/Transport 문제, uri=" + cfg_.rtspSetupUri + ")");
        return false;
    }

    sessionId_ = res2.substr(sessPos + 9, res2.find_first_of(";\r\n", sessPos) - (sessPos + 9));

    // Session 헤더 '한 줄' 전체를 떼어 timeout= 을 읽는다.
    // sessionId_ 는 ';' 에서 잘리므로 timeout 은 그 뒤에 남아 있고, 예전에는 통째로 버려졌다
    const std::size_t sessLineEnd = res2.find("\r\n", sessPos);
    deriveKeepAliveInterval(
        res2.substr(sessPos, (sessLineEnd == std::string::npos ? res2.size() : sessLineEnd) - sessPos));

    // 인터리브 채널은 '요청'이 아니라 '응답'이 확정한다.
    // 응답을 무시하고 0 을 가정하면, 서버가 2-3 을 배정한 경우 메타데이터를 통째로 놓치거나
    // (channel != 0 으로 전부 버림) 엉뚱한 트랙을 ONVIF 파서에 먹이게 된다
    const std::size_t ilPos = res2.find("interleaved=");
    if (ilPos != std::string::npos) {
        const int announced = std::atoi(res2.c_str() + ilPos + 12);
        if (announced >= 0 && announced <= 255) {
            rtpChannel_ = announced;
        }
    }

    logSuccess(kIface,
               "인증 성공, 세션 ID: " + sessionId_ + ", 인터리브 채널=" + std::to_string(rtpChannel_) +
                   ", 세션 타임아웃=" + (sessionTimeoutSec_ > 0 ? std::to_string(sessionTimeoutSec_) + "s" : "미통보") +
                   ", keep-alive 주기=" + std::to_string(effectiveKeepAliveSec_) + "s");
    return true;
}

void RtspClientV2::deriveKeepAliveInterval(const std::string& sessionLine) {
    sessionTimeoutSec_ = 0;

    const std::size_t toPos = sessionLine.find("timeout=");
    if (toPos != std::string::npos) {
        sessionTimeoutSec_ = std::atoi(sessionLine.c_str() + toPos + 8);
    }

    // 통보가 없으면 설정값을 그대로 쓴다 (기존 동작)
    effectiveKeepAliveSec_ = keepAliveIntervalSec_;

    if (sessionTimeoutSec_ > 0) {
        // 절반 주기: 한 번 유실돼도 다음 keep-alive 가 만료 전에 도착한다
        const int safeInterval = sessionTimeoutSec_ / 2;
        effectiveKeepAliveSec_ = std::min(keepAliveIntervalSec_, safeInterval);

        if (effectiveKeepAliveSec_ < keepAliveIntervalSec_) {
            logSuccess(kIface, "카메라가 세션 타임아웃 " + std::to_string(sessionTimeoutSec_) +
                                   "s 를 통보 - keep-alive 주기를 설정값(" + std::to_string(keepAliveIntervalSec_) +
                                   "s)에서 " + std::to_string(effectiveKeepAliveSec_) + "s 로 낮춤");
        }
    }

    // 0 이면 keepAliveLoop 의 sleep 루프가 돌지 않아 폭주하므로 반드시 1 이상
    if (effectiveKeepAliveSec_ < 1) {
        effectiveKeepAliveSec_ = 1;
    }
}

void RtspClientV2::play() {
    std::string auth = buildDigestHeader("PLAY", cfg_.rtspPlayUri);
    std::string req =
        "PLAY " + cfg_.rtspPlayUri + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + sessionId_ + "\r\n" + auth + "\r\n";
    if (send(sock_, req.c_str(), req.length(), MSG_NOSIGNAL) < 0) {
        logError(kIface, std::string("PLAY 요청 전송 실패 - ") + std::strerror(errno));
        return;
    }

    std::string res;
    if (!recvHeaders(res)) {
        logError(kIface, "PLAY 응답 수신 실패 (uri=" + cfg_.rtspPlayUri + ")");
        return;
    }

    // PLAY 응답을 반드시 확인 - 200이 아니면 카메라가 곧바로 연결을 끊어 run()이 즉시 종료됨
    // (예: PLAY URL이 aggregate control URL과 다르면 404/455)
    const std::string status = statusLine(res);
    if (status.find(" 200") == std::string::npos) {
        logError(kIface, "PLAY 실패 - 응답: [" + status + "] (uri=" + cfg_.rtspPlayUri +
                             ") PLAY는 트랙이 아니라 세션 aggregate URL(끝의 trackID/슬래시 없이)로 보내야 함");
        return;
    }

    logSuccess(kIface, "PLAY 성공, 스트리밍 시작 - 응답: [" + status + "]");

    playOk_ = true;  // 여기까지 와야 스트리밍이 실제로 시작됨 (workerLoop 가 run() 진입 판단에 사용)
    keepRunning_ = true;
    keepaliveThread_ = std::thread(&RtspClientV2::keepAliveLoop, this);
}

std::size_t RtspClientV2::rtpHeaderLength(const std::uint8_t* packet, std::size_t packetLen) {
    if (packetLen < static_cast<std::size_t>(kRtpHeaderSize)) {
        return 0;
    }

    // 버전 검사: RTP 가 아니면(예: 스트림 desync 로 엉뚱한 바이트를 읽은 경우) 즉시 거른다
    if (static_cast<std::uint8_t>((packet[0] >> 6) & 0x03) != kRtpVersion) {
        return 0;
    }

    // 고정 헤더 + CSRC 목록 (첫 바이트 하위 4비트 = CSRC 개수, 항목당 4바이트)
    std::size_t headerLen = static_cast<std::size_t>(kRtpHeaderSize) + 4u * static_cast<std::size_t>(packet[0] & 0x0F);
    if (packetLen < headerLen) {
        return 0;
    }

    // 확장 헤더 (X 비트): 4바이트 헤더 + (16비트 length 필드 x 4바이트)
    if ((packet[0] & 0x10) != 0) {
        if (packetLen < headerLen + 4u) {
            return 0;
        }
        const std::size_t extWords =
            (static_cast<std::size_t>(packet[headerLen + 2]) << 8) | static_cast<std::size_t>(packet[headerLen + 3]);
        headerLen += 4u + 4u * extWords;
        if (packetLen < headerLen) {
            return 0;
        }
    }

    return headerLen;
}

void RtspClientV2::run() {
    std::string metadataBuffer;
    metadataBuffer.reserve(8192);

    // 취소 플래그를 매 반복 확인한다. 스트림이 건강하면 recv() 가 계속 성공해 타임아웃이 나지
    // 않으므로, 이 확인이 없으면 run() 은 영영 반환하지 않고 워커 join 이 무한 대기한다.
    while (!cancelled_.load(std::memory_order_acquire)) {
        std::uint8_t sync = 0;
        if (!readByte(sync)) {
            break;
        }

        if (sync != '$') {
            continue;
        }

        std::uint8_t header[3];
        if (!readBytes(header, 3)) {
            break;
        }

        const int channel = header[0];
        const int payloadLen = (header[1] << 8) | header[2];

        if (static_cast<std::size_t>(payloadLen) > kMaxRtpPayloadSize) {
            break;
        }

        if (!readBytes(rtpPacket_.data(), static_cast<std::size_t>(payloadLen))) {
            break;
        }

        // 메타데이터 트랙이 실린 채널만 통과시킨다. SETUP 응답이 배정한 값이며,
        // 나머지(RTCP, 다른 트랙)는 여기서 버려야 ONVIF 파서에 비-메타데이터가 들어가지 않는다
        if (channel != rtpChannel_) {
            continue;
        }

        // RTP 헤더는 고정 12바이트가 아니다 -- CSRC/확장 헤더만큼 길어진다.
        // 고정 12로 자르면 페이로드 앞에 이진 쓰레기가 붙어 XML 이 깨진다
        const std::size_t rtpHeaderLen = rtpHeaderLength(rtpPacket_.data(), static_cast<std::size_t>(payloadLen));
        if (rtpHeaderLen == 0 || static_cast<std::size_t>(payloadLen) <= rtpHeaderLen) {
            continue;  // RTP 가 아니거나 페이로드가 비어 있음
        }

        if (metadataBuffer.empty()) {
            frameAssembleStart_ = std::chrono::steady_clock::now();
        }

        const bool marker = (rtpPacket_[1] & 0x80) != 0;
        metadataBuffer.append(reinterpret_cast<const char*>(rtpPacket_.data() + rtpHeaderLen),
                              static_cast<size_t>(payloadLen) - rtpHeaderLen);

        if (metadataBuffer.size() > maxMetadataFrameSize_) {
            // marker bit가 누락됐거나 스트림이 손상된 것으로 간주 -> 무한정 쌓이기 전에
            // 버퍼를 버리고 연결 자체를 재수립 (상위 Source의 재연결 백오프에 위임)
            logError(kIface, "metadata 프레임 크기가 상한(" + std::to_string(maxMetadataFrameSize_) +
                                 "B)을 초과 (marker 누락 의심) - 연결 재수립");
            break;
        }

        if (marker) {
            const auto elapsed = std::chrono::steady_clock::now() - frameAssembleStart_;
            metrics_.totalAssembleTime += elapsed;
            metrics_.totalBytes += metadataBuffer.size();
            ++metrics_.payloadCount;

            if (onPayloadReceived) {
                onPayloadReceived(std::string_view(metadataBuffer));
            }

            metadataBuffer.clear();

            reportMetricsIfDue();
        }
    }

    keepRunning_ = false;
    if (cancelled_.load(std::memory_order_acquire)) {
        // 의도된 종료 -- 오류가 아니므로 error 로 남기지 않음 (종료 때마다 오탐 로그가 쌓이던 문제)
        logSuccess(kIface, "취소 요청으로 스트림 루프 종료");
    } else {
        logError(kIface, "스트림 종료 또는 연결 끊김");
    }
}

void RtspClientV2::reportMetricsIfDue() {
    using namespace std::chrono;

    const auto now = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(now - metrics_.windowStart);
    if (elapsed < metricsReportInterval_ || metrics_.payloadCount == 0) {
        return;
    }

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
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        snprintf(output + i * 2, 3, "%02x", hash[i]);
    }

    return std::string(output);
}

std::string RtspClientV2::buildDigestHeader(const std::string& method, const std::string& uri) {
    char ncBuf[9];
    snprintf(ncBuf, sizeof(ncBuf), "%08x", nonceCount_++);
    const std::string nc(ncBuf);
    const std::string& cnonce = cnonce_;
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
        // 1초씩 쪼개서 자는 이유: 종료 요청(keepRunning_=false)에 최대 1초 안에 반응하기 위함
        for (int i = 0; i < effectiveKeepAliveSec_ && keepRunning_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!keepRunning_) {
            break;
        }

        std::string auth = buildDigestHeader("GET_PARAMETER", cfg_.rtspPlayUri);
        std::string req = "GET_PARAMETER " + cfg_.rtspPlayUri + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq_++) +
                          "\r\nSession: " + sessionId_ + "\r\n" + auth + "\r\n";

        if (send(sock_, req.c_str(), req.length(), MSG_NOSIGNAL) < 0) {
            logError(kIface, "Keep-alive 전송 실패 - 연결 끊김 추정");
            break;
        }
    }
}
