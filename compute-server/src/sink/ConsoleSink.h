#pragma once

/**
 * @file    ConsoleSink.h
 * @brief   Console에 출력 (Debug)
 */

#include <iostream>
#include <string_view>

#include "Contract.h"
#include "interfaces/ISink.h"

class ConsoleTopViewSink : public ISink<veda::TopViewFrame> {
public:
    void send(const veda::TopViewFrame& f) override {
        std::cout << "[RISK] ch=" << f.ch << " ts=" << f.ts << " objects=" << f.objects.size() << "\n";
        for (const auto& o : f.objects) {
            std::cout << "    id=" << o.id << " cls=" << veda::toString(o.cls) << " world=("
                      << o.pos.x << "," << o.pos.y << ")"
                      << " edge=" << o.edge << "\n";
        }
    }
};

class ConsoleBlurSink : public ISink<veda::BlurFrame> {
public:
    void send(const veda::BlurFrame& f) override {
        std::cout << "[BLUR] ch=" << f.ch << " ts=" << f.ts << " blurs=" << f.blurs.size() << "\n";
        for (const auto& b : f.blurs) {
            std::cout << "    id=" << b.id << " cls=" << veda::toString(b.cls) << " box=[" << b.box.l << "," << b.box.t
                      << "," << b.box.r << "," << b.box.b << "]\n";
        }
    }
};