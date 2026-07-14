#include "network/RtspClient.h"

#include <arpa/inet.h>
#include <openssl/md5.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

RtspClient::RtspClient(const AppConfig& config) : cfg_(config) {}

RtspClient::~RtspClient() {
    keepRunning_ = false;
    if (keepaliveThread_.joinable())
        keepaliveThread_.join();
    if (sock_ != -1) {
        close(sock_);
        std::cout << "[*] Socket closed cleanly.\n";
    }
}

bool RtspClient::connect() {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        std::cerr << "[-] Socket creation failed\n";
        return false;
    }

    struct timeval tv {
        5, 0
    };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(cfg_.rtspPort));
    inet_pton(AF_INET, cfg_.rtspIp.c_str(), &serverAddr.sin_addr);

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        std::cerr << "[-] Connect failed\n";
        return false;
    }
    std::cout << "[*] Connected to Camera (" << cfg_.rtspIp << ":" << cfg_.rtspPort << ")\n";
    return true;
}

bool RtspClient::recvHeaders(std::string& out) {
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

bool RtspClient::setup() {
    std::string req1 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 1\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
    send(sock_, req1.c_str(), req1.length(), 0);

    std::string res1;
    if (!recvHeaders(res1))
        return false;

    const size_t realmPos = res1.find("realm=\"");
    const size_t noncePos = res1.find("nonce=\"");
    if (realmPos != std::string::npos)
        realm_ = res1.substr(realmPos + 7, res1.find('"', realmPos + 7) - (realmPos + 7));
    if (noncePos != std::string::npos)
        nonce_ = res1.substr(noncePos + 7, res1.find('"', noncePos + 7) - (noncePos + 7));
    if (nonce_.empty())
        return false;

    std::string auth = buildDigestHeader("SETUP", cfg_.rtspSetupUri);
    std::string req2 = "SETUP " + cfg_.rtspSetupUri +
                       " RTSP/1.0\r\nCSeq: 2\r\n"
                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" +
                       auth + "\r\n";
    send(sock_, req2.c_str(), req2.length(), 0);

    std::string res2;
    if (!recvHeaders(res2))
        return false;

    const size_t sessPos = res2.find("Session: ");
    if (sessPos == std::string::npos)
        return false;

    sessionId_ = res2.substr(sessPos + 9, res2.find_first_of(";\r\n", sessPos) - (sessPos + 9));
    std::cout << "[*] Successfully Auth! Session ID: " << sessionId_ << "\n";
    return true;
}

void RtspClient::play() {
    std::string auth = buildDigestHeader("PLAY", cfg_.rtspPlayUri);
    std::string req =
        "PLAY " + cfg_.rtspPlayUri + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + sessionId_ + "\r\n" + auth + "\r\n";
    send(sock_, req.c_str(), req.length(), 0);

    std::string res;
    recvHeaders(res);
    std::cout << "[*] PLAY command sent. Stream starting...\n";

    keepRunning_ = true;
    keepaliveThread_ = std::thread(&RtspClient::keepAliveLoop, this);
}

void RtspClient::run() {
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> rtpPacket;
    std::string metadataBuffer;

    while (true) {
        if (!readExact(header, 1))
            break;
        if (header[0] != '$')
            continue;

        if (!readExact(header, 3))
            break;
        const int channel = header[0];
        const int payloadLen = (header[1] << 8) | header[2];

        if (!readExact(rtpPacket, static_cast<size_t>(payloadLen)))
            break;
        if (channel != 0)
            continue;
        if (payloadLen < kRtpHeaderSize)
            continue;

        const bool marker = (rtpPacket[1] & 0x80) != 0;
        metadataBuffer.append(reinterpret_cast<const char*>(rtpPacket.data() + kRtpHeaderSize),
                              static_cast<size_t>(payloadLen - kRtpHeaderSize));

        if (marker) {
            if (onPayloadReceived)
                onPayloadReceived(std::string_view(metadataBuffer));
            metadataBuffer.clear();
        }
    }

    keepRunning_ = false;
    std::cout << "[-] Stream ended or connection lost.\n";
}

std::string RtspClient::md5Hex(const std::string& input) {
    unsigned char hash[MD5_DIGEST_LENGTH];
    // NOLINTNEXTLINE
    MD5(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    char output[33];
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        snprintf(output + i * 2, 3, "%02x", hash[i]);
    return std::string(output);
}

std::string RtspClient::buildDigestHeader(const std::string& method, const std::string& uri) {
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

bool RtspClient::readExact(std::vector<std::uint8_t>& buf, std::size_t len) {
    buf.resize(len);
    std::size_t totalRead = 0;
    while (totalRead < len) {
        const ssize_t n = recv(sock_, buf.data() + totalRead, len - totalRead, 0);
        if (n <= 0)
            return false;
        totalRead += static_cast<size_t>(n);
    }
    return true;
}

void RtspClient::keepAliveLoop() {
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
            std::cerr << "[-] Keep-alive send failed. Connection likely lost.\n";
            break;
        }
    }
}