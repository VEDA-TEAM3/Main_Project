#pragma once

/**
 * @file ConsoleSink.h
 * @brief Console 출력 및 BlurFrame MQTT 전달
 */

#include <iostream>

#include "Contract.h"
#include "interfaces/ISink.h"

class ConsoleTopViewSink : public ISink<veda::TopViewFrame> {
public:
    void send(const veda::TopViewFrame& frame) override {
        std::cout << "[RISK] ch=" << frame.ch << " ts=" << frame.ts << " objects=" << frame.objects.size() << "\n";

        for (const auto& object : frame.objects) {
            std::cout << "    id=" << object.id << " cls=" << veda::toString(object.cls) << " world=(" << object.pos.x
                      << "," << object.pos.y << ") edge=" << object.edge << "\n";
        }

        /*
         * TopViewFrame은 MQTT로 보내지 않는다.
         */
    }
};

class ConsoleBlurSink : public ISink<veda::BlurFrame> {
public:
    void send(const veda::BlurFrame& frame) override {
        std::cout << "[BLUR] ch=" << frame.ch << " ts=" << frame.ts << " blurs=" << frame.blurs.size() << "\n";

        for (const auto& blur : frame.blurs) {
            std::cout << "    id=" << blur.id << " cls=" << veda::toString(blur.cls) << " box=[" << blur.box.l << ","
                      << blur.box.t << "," << blur.box.r << "," << blur.box.b << "]\n";
        }
    }
};
