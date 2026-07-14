#include "source/RtspOnvifSource.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#include "network/RtspClient.h"

RtspOnvifSource::RtspOnvifSource(const AppConfig& config) : config_(config) {
    worker_ = std::thread(&RtspOnvifSource::workerLoop, this);
}

RtspOnvifSource::~RtspOnvifSource() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void RtspOnvifSource::workerLoop() {
    int backoffSec = 1;
    constexpr int kMaxBackoffSec = 30;

    while (!stopping_) {
        RtspClient client(config_);
        client.onPayloadReceived = [this](std::string_view payload) {
            domain::RawPacket pkt;
            pkt.channelId = config_.channelId;
            pkt.bytes.assign(payload.begin(), payload.end());
            pkt.recvTime = std::chrono::system_clock::now();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                queue_.push(std::move(pkt));
            }
            cv_.notify_one();
        };

        if (client.connect() && client.setup()) {
            client.play();
            backoffSec = 1;
            client.run();
        }

        if (stopping_)
            break;

        std::cerr << "[RtspOnvifSource] ch=" << config_.channelId << " disconnected. retry in " << backoffSec << "s\n";

        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::seconds(backoffSec), [this] { return stopping_.load(); });
        backoffSec = std::min(backoffSec * 2, kMaxBackoffSec);
    }
}

bool RtspOnvifSource::next(domain::RawPacket& out) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return !queue_.empty() || stopping_.load(); });

    if (queue_.empty())
        return false;

    out = std::move(queue_.front());
    queue_.pop();
    return true;
}