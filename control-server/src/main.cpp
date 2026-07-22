#include <iostream>

#include "core/AppConfig.h"
#include "core/AppContext.h"
#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Main";
}  // namespace

int main() {
    AppConfig config = AppConfig::load("config.json");

    AppContext context(config);
    auto controller = context.buildController();

    controller->start();
    logSuccess(kIface, "===control-server start===");

    std::cout << "If you want to stop, you push Enter...\n";
    std::cin.get();

    controller->stop();
    logSuccess(kIface, "===control-server stop===");

    return 0;
}