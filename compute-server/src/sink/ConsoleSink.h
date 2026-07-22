#pragma once

/**
 * @file    ConsoleSink.h
 * @brief   콘솔 출력 전용 Sink
 * @details TopView/Blur 둘 다 콘솔에만 출력하고 MQTT 등 다른 전송 계층과는 무관함
 *          (BlurFrame을 실제로 MQTT에 발행하는 건 sink/MqttBlurSink.h가 전담)
 */

#include <iostream>

#include "Contract.h"
#include "interfaces/ISink.h"

class ConsoleTopViewSink final : public ISink<veda::TopViewFrame> {
public:
    void send(const veda::TopViewFrame& frame) override {
        std::cout << "[RISK] ch=" << frame.ch << " ts=" << frame.ts << " objects=" << frame.objects.size() << "\n";

        for (const auto& object : frame.objects) {
            std::cout << "    id=" << object.id << " cls=" << veda::toString(object.cls) << " world=("
                      << object.pos.x << "," << object.pos.y << ") edge=" << object.edge << "\n";
        }
    }
};

class ConsoleBlurSink final : public ISink<veda::BlurFrame> {
public:
    void send(const veda::BlurFrame& frame) override {
        std::cout << "[BLUR] ch=" << frame.ch << " ts=" << frame.ts << " blurs=" << frame.blurs.size() << "\n";

        for (const auto& blur : frame.blurs) {
            std::cout << "    id=" << blur.id << " cls=" << veda::toString(blur.cls) << " box=[" << blur.box.l << ","
                      << blur.box.t << "," << blur.box.r << "," << blur.box.b << "]\n";
        }
    }
};
