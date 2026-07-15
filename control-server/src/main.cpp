#include <iostream>

#include "core/AppConfig.h"
#include "core/AppContext.h"

int main() {
    AppConfig config = AppConfig::load("config.json");

    AppContext context(config);
    auto controller = context.buildController();

    controller->start();

    return 0;
}