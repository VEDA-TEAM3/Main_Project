#include <iostream>

#include "core/AppConfig.h"
#include "core/AppContext.h"

int main() {
    AppConfig config = AppConfig::load("config.json");

    AppContext context(config);
    auto controller = context.buildController();

    controller->start();

    std::cout << "종료하려면 Enter 키를 누르세요.\n";
    std::cin.get();

    controller->stop();

    return 0;
}