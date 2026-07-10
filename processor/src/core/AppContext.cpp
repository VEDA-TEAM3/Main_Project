/**
 * @file    AppContext.cpp
 * @brief   AppContext 구현
 */

#include "core/AppContext.h"

bool AppContext::initialize() {
    network_ = createNetwork(config_);
    parser_ = createParser();
    trans_ = createTrans();
    sender_ = createSender();

    if (!network_ || !parser_ || !trans_ || !sender_) {
        std::cerr << "[-] Failed to initialize one or more components.\n";
        return false;
    }

    return true;
}