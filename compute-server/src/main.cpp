/**
 * @file    main.cpp
 * @brief   연산 서버 진입점
 *
 * @details
 * Source에서 RawPacket을 당겨 Pipeline에 넣는 루프만 담당
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
 * 시그널 핸들러 대신 전용 스레드의 sigwait()을 쓰는 이유:
 * 핸들러 안에서는 async-signal-safe 함수만 부를 수 있는데, 종료 처리에는
 * mutex/condition_variable 조작(IMetadataSource::stop)이 필요함
 * -- sigwait은 일반 스레드 컨텍스트라 그런 제약이 없음
 *
 * @warning main 진입 직후, 어떤 스레드도 만들어지기 전에 시그널을 블록해야 함
 *          (스레드는 생성 시점의 시그널 마스크를 물려받으므로, 그래야 다른
 *           스레드가 시그널을 가로채지 않고 이 스레드만 받게 됨)
 */
void waitForShutdownSignal(sigset_t mask, IMetadataSource& source) {
    while (true) {
        int signalNumber = 0;
        if (sigwait(&mask, &signalNumber) != 0) {
            logError(kIface, "sigwait 실패 - 시그널 감시 스레드를 종료함");
            return;
        }

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
    sigset_t shutdownMask;
    sigemptyset(&shutdownMask);
    sigaddset(&shutdownMask, SIGINT);
    sigaddset(&shutdownMask, SIGTERM);

    sigaddset(&shutdownMask, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &shutdownMask, nullptr) != 0) {
        logError(kIface, "시그널 마스크 설정 실패 - 정상 종료를 보장할 수 없습니다");
    }

    std::signal(SIGPIPE, SIG_IGN);

    const AppConfig config = AppConfig::load("config.json");

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

    domain::RawPacket raw;
    std::uint64_t packetCount = 0;
    while (context->source().next(raw)) {
        ++packetCount;
        try {
            context->pipeline().onPacket(raw);
        } catch (const std::exception& error) {
            logError(kIface, std::string("패킷 처리 중 예외 - 이 패킷을 건너뜁니다: ") + error.what());
        } catch (...) {
            logError(kIface, "패킷 처리 중 알 수 없는 예외 - 이 패킷을 건너뜁니다");
        }
    }

    pthread_kill(signalThread.native_handle(), SIGTERM);
    signalThread.join();

    context.reset();

    logSuccess(kIface, "정상 종료 (총 " + std::to_string(packetCount) + "개 패킷 처리)");
    return 0;
}