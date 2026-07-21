#pragma once

/**
 * @file    ConsoleSink.h
 * @brief   Console 출력 및 MQTT 전달
 */

#include <iostream>
#include <string_view>

#include "Contract.h"
#include "interfaces/ISink.h"
#include "MqttSink.h"

class ConsoleTopViewSink
    : public ISink<veda::TopViewFrame> {
public:
    void send(
        const veda::TopViewFrame& frame
    ) override {
        /*
         * 기존 Console 출력은 그대로 유지합니다.
         */
        std::cout
            << "[RISK] ch=" << frame.ch
            << " ts=" << frame.ts
            << " objects=" << frame.objects.size()
            << "\n";

        for (const auto& object : frame.objects) {
            std::cout
                << "    id=" << object.id
                << " cls=" << veda::toString(object.cls)
                << " world=("
                << object.pos.x
                << ","
                << object.pos.y
                << ") edge="
                << object.edge
                << "\n";
        }

        /*
         * Console이 받은 동일한 프레임을 MQTT에도 전달합니다.
         */
        publishTopViewToMqtt(frame);
    }
};

class ConsoleBlurSink
    : public ISink<veda::BlurFrame> {
public:
    void send(
        const veda::BlurFrame& frame
    ) override {
        std::cout
            << "[BLUR] ch=" << frame.ch
            << " ts=" << frame.ts
            << " blurs=" << frame.blurs.size()
            << "\n";

        for (const auto& blur : frame.blurs) {
            std::cout
                << "    id=" << blur.id
                << " cls=" << veda::toString(blur.cls)
                << " box=["
                << blur.box.l
                << ","
                << blur.box.t
                << ","
                << blur.box.r
                << ","
                << blur.box.b
                << "]\n";
        }
    }
};