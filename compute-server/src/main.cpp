#include <iostream>

#include "core/AppConfig.h"
#include "core/AppContext.h"

int main() {
    std::cout << "[*] Starting Compute Server...\n";

    AppConfig config = AppConfig::load("config.json");

    AppContext ctx(config);

    domain::RawPacket raw;
    int packetCount = 0;

    std::cout << "[*] Pipeline assembled. Waiting for RTSP streams...\n";

    while (ctx.source().next(raw)) {
        packetCount++;
        std::cout << "--- packet " << packetCount << " ---\n";

        ctx.pipeline().onPacket(raw);
    }

    std::cout << "[-] Stream ended. Total " << packetCount << " packets processed.\n";
    return 0;
}