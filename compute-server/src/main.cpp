#include <iostream>

#include "core/AppConfig.h"
#include "core/AppContext.h"

int main() {
    std::cout << "[*] Starting Compute Server...\n";

    // 1. .env 파일에서 설정 로드 (헤더 온리 함수 사용)
    AppConfig config = loadAppConfig(".env");

    // 2. AppContext가 설정값을 바탕으로 파이프라인 조립 (RTSP 연결 포함)
    AppContext ctx(config);

    domain::RawPacket raw;
    int packetCount = 0;

    std::cout << "[*] Pipeline assembled. Waiting for RTSP streams...\n";

    // 3. IMetadataSource::next()를 통해 실시간 RTSP 스트림 풀링
    // 연결이 잠시 끊기더라도 RtspOnvifSource 내부에서 재연결을 시도하며 대기함
    while (ctx.source().next(raw)) {
        packetCount++;
        std::cout << "--- packet " << packetCount << " ---\n";

        // 4. 수신된 패킷을 파이프라인으로 흘려보냄
        ctx.pipeline().onPacket(raw);
    }

    std::cout << "[-] Stream ended. Total " << packetCount << " packets processed.\n";
    return 0;
}