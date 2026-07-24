/**
 * @file    main.cpp
 * @brief   연산 서버 진입점
 *
 * @details
 * Source에서 RawPacket을 당겨 Pipeline에 넣는 루프만 담당
 * (네트워크/Source 계층은 Pipeline 밖에 두는 설계 -- Pipeline.h 참고)
 */

#include <pthread.h>

#include <csignal>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "Logger.h"
#include "core/AppConfig.h"
#include "core/AppContext.h"

namespace {

constexpr const char* kIface = "Main";

/**
 * @brief   종료 시그널을 기다렸다가 Source를 깨우는 전용 스레드 루틴
 *
 * @details
 * 시그널 '핸들러' 대신 전용 스레드의 sigwait()을 쓰는 이유:
 * 핸들러 안에서는 async-signal-safe 함수만 부를 수 있는데, 종료 처리에는
 * mutex/condition_variable 조작(IMetadataSource::stop)이 필요함
 * -- sigwait은 일반 스레드 컨텍스트라 그런 제약이 없음
 *
 * @warning main 진입 직후, 어떤 스레드도 만들어지기 전에 시그널을 블록해야 함
 *          (스레드는 생성 시점의 시그널 마스크를 물려받으므로, 그래야 다른
 *           스레드가 시그널을 가로채지 않고 이 스레드만 받게 됨)
 */
void waitForShutdownSignal(sigset_t mask, IMetadataSource& source) {
    for (;;) {
        int signalNumber = 0;
        if (sigwait(&mask, &signalNumber) != 0) {
            logError(kIface, "sigwait 실패 - 시그널 감시 스레드를 종료함");
            return;
        }

        // SIGHUP: logrotate 가 파일을 rename 한 뒤 postrotate 로 보내는 신호 -> 새 파일로 다시 열고
        // 계속 대기한다 (종료 신호가 아님). 실제 close/open 은 로그 워커가 다음 주기에 수행
        if (signalNumber == SIGHUP) {
            logSuccess(kIface, "SIGHUP 수신 - 로그 파일 재오픈 (logrotate)");
            reopenLogFile();
            continue;
        }

        logSuccess(kIface, "종료 시그널 수신 (signal=" + std::to_string(signalNumber) + ") - 정리 후 종료합니다");
        source.stop();
        return;
    }
}

}  // namespace

int main() {
    // 1) 시그널 블록 -- 반드시 스레드 생성 이전에
    sigset_t shutdownMask;
    sigemptyset(&shutdownMask);
    sigaddset(&shutdownMask, SIGINT);
    sigaddset(&shutdownMask, SIGTERM);
    // SIGHUP 은 logrotate 의 postrotate 신호. 기본 처리 동작이 '프로세스 종료'라서,
    // 여기서 블록해 두지 않으면 로그가 회전될 때마다 서버가 죽는다
    sigaddset(&shutdownMask, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &shutdownMask, nullptr) != 0) {
        logError(kIface, "시그널 마스크 설정 실패 - 정상 종료를 보장할 수 없습니다");
    }

    const AppConfig config = AppConfig::load("config.json");

    // 설정을 읽자마자 로거부터 구성 (이 줄 이전의 로그는 기본값 Info/콘솔+파일로 나감)
    LogConfig logConfig;
    logConfig.level = logLevelFromString(config.logLevel);
    logConfig.console = config.logToConsole;
    logConfig.file = config.logToFile;
    logConfig.flushIntervalMs = config.logFlushIntervalMs;
    logConfig.maxPendingEntries = config.logMaxPendingEntries;
    logConfig.fileName = config.logFileName;
    initLogger(logConfig);

    logSuccess(kIface,
               "연산 서버 시작 (logLevel=" + config.logLevel + ", ch=" + std::to_string(config.channelId) + ")");

    // 2) 조립 단계의 설정 오류는 여기서 잡음
    //    AppConfig::load 는 예외를 던지지 않지만, 구현체 생성자(HomographyTransform,
    //    AffineImageCoordinateMapper)는 설정값이 구조적으로 잘못되면 던짐
    //    -- 잡지 않으면 std::terminate 로 죽어서 원인이 로그에 남지 않음
    //
    //    [ 왜 기본값으로 폴백하지 않는가 ]
    //    호모그래피가 잘못된 채로 계속 돌면 '그럴듯하지만 틀린' 월드 좌표를 계속
    //    발행하게 됨. 위험 판단의 입력이므로 조용히 틀리느니 즉시 죽는 편이 안전함
    std::unique_ptr<AppContext> context;
    try {
        context = std::make_unique<AppContext>(config);
    } catch (const std::exception& error) {
        logError(kIface, std::string("초기화 실패 - config.json 설정을 확인하세요: ") + error.what());
        return 1;
    } catch (...) {
        logError(kIface, "초기화 실패 - 알 수 없는 예외");
        return 1;
    }

    std::thread signalThread(waitForShutdownSignal, shutdownMask, std::ref(context->source()));

    logSuccess(kIface, "파이프라인 조립 완료 - RTSP 스트림 대기 중");

    // 3) 메인 루프
    domain::RawPacket raw;
    std::uint64_t packetCount = 0;
    while (context->source().next(raw)) {
        ++packetCount;
        context->pipeline().onPacket(raw);
    }

    // 4) 스트림이 시그널 없이 먼저 끝났을 수도 있으므로 감시 스레드를 깨워서 회수
    //    (이미 시그널을 받아 빠져나온 경우에도 sigwait 은 이미 반환했으므로 무해)
    pthread_kill(signalThread.native_handle(), SIGTERM);
    signalThread.join();

    // 5) 명시적 파괴: Sink 워커 정지, MQTT 종료 신호("0") 발행, 스레드 join 이 여기서 일어남
    //    (context 를 unique_ptr 로 둔 이유이기도 함)
    //
    //    [ 주의 ] Sink 큐는 flush 되지 '않는다'. MqttFrameSink::shutdown() 은 남아 있는 프레임을
    //    drop 으로 집계하며 버린다 -- 실시간 좌표라 종료 시점의 잔여 프레임은 가치가 없고,
    //    브로커가 죽어 있으면 flush 가 무한정 늘어질 수 있기 때문.
    //    => SIGINT/SIGTERM 시 '마지막 프레임 전달'은 보장되지 않는다.
    //       보장이 필요해지면 shutdown() 에 드레인 + 타임아웃을 넣어야 함 (지금은 의도적 미보장)
    context.reset();

    logSuccess(kIface, "정상 종료 (총 " + std::to_string(packetCount) + "개 패킷 처리)");
    return 0;
}
