/**
 * @file    Processor.cpp
 * @brief   Processor 구현
 */
#include "core/Processor.h"

#include <sstream>

Processor::Processor(std::shared_ptr<INetwork> network, std::shared_ptr<IParser> parser, std::shared_ptr<ITrans> trans,
                     std::shared_ptr<ISender> sender)
    : network_(std::move(network)), parser_(std::move(parser)), trans_(std::move(trans)), sender_(std::move(sender)) {
    network_->onPayloadReceived = [this](std::string_view data) { onPayload(data); };
}

bool Processor::run() {
    if (!network_->connect())
        return false;
    if (!network_->setup())
        return false;
    network_->play();
    network_->run();
    return true;
}

void Processor::onPayload(std::string_view raw) {
    ParsedFrame frame = parser_->parse(raw);
    if (frame.objects.empty())
        return;

    std::vector<DetectedObject> filtered;
    for (const auto& obj : frame.objects) {
        if (obj.type == "Human" || obj.type == "Vehicle") {
            filtered.push_back(obj);
        }
    }
    if (filtered.empty())
        return;

    std::vector<WorldPoint> worldPoints;
    worldPoints.reserve(filtered.size());
    for (const auto& obj : filtered) {
        worldPoints.push_back(trans_->transform(obj.cx, obj.cy));
    }

    std::string packet = buildPacket(frame.timestamp_ms, filtered, worldPoints);

    std::cout << "[Packet] " << packet << "\n";

    sender_->send(packet);
}

std::string Processor::buildPacket(int64_t timestamp_ms, const std::vector<DetectedObject>& objects,
                                   const std::vector<WorldPoint>& worldPoints) {
    std::ostringstream oss;
    oss << "{\"timestamp\":" << timestamp_ms << ",\"objects\":[";
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        const auto& world = worldPoints[i];
        if (i > 0)
            oss << ",";
        oss << "{\"id\":" << obj.id << ",\"type\":\"" << obj.type << "\""
            << ",\"x\":" << world.x << ",\"y\":" << world.y << ",\"dist\":" << world.distance << "}";
    }
    oss << "]}";
    return oss.str();
}