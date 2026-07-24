/**
 * @file    main.cpp
 * @brief   관제 서버 진입점
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
 * @brief   종료 시그널을 기다렸다가 Controller 를 멈추는 전용 스레드 루틴
 *
 * @details
 * 시그널 '핸들러' 대신 전용 스레드의 sigwait()을 쓰는 이유:
 * 핸들러 안에서는 async-signal-safe 함수만 부를 수 있는데, 종료 처리에는
 * mutex/condition_variable 조작(Controller::stop -> receiver->stop)이 필요함
 *
 * @warning main 진입 직후, 어떤 스레드도 만들어지기 전에 시그널을 블록해야 함
 *          (스레드는 생성 시점의 시그널 마스크를 물려받음)
 */
void waitForShutdownSignal(sigset_t mask, bool& stopRequested) {
    for (;;) {
        int signalNumber = 0;
        if (sigwait(&mask, &signalNumber) != 0) {
            logError(kIface, "sigwait 실패 - 시그널 감시 스레드를 종료함");
            return;
        }

        // SIGHUP: logrotate 의 postrotate 신호 -> 로그 파일만 다시 열고 계속 대기 (종료 아님)
        if (signalNumber == SIGHUP) {
            logSuccess(kIface, "SIGHUP 수신 - 로그 파일 재오픈 (logrotate)");
            reopenLogFile();
            continue;
        }

        logSuccess(kIface, "종료 시그널 수신 (signal=" + std::to_string(signalNumber) + ") - 정리 후 종료합니다");
        stopRequested = true;
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

    // 설정을 읽자마자 로거부터 구성 (shared/Logger.h — compute-server와 동일한 방식)
    LogConfig logConfig;
    logConfig.level = logLevelFromString(config.logLevel);
    logConfig.console = config.logToConsole;
    logConfig.file = config.logToFile;
    logConfig.flushIntervalMs = config.logFlushIntervalMs;
    logConfig.maxPendingEntries = config.logMaxPendingEntries;
    logConfig.fileName = config.logFileName;
    initLogger(logConfig);

    // 2) 조립 단계의 설정 오류를 여기서 잡음
    //    (잡지 않으면 std::terminate 로 죽어서 원인이 로그에 남지 않음)
    std::shared_ptr<Controller> controller;
    try {
        AppContext context(config);
        controller = context.buildController();
        controller->start();
    } catch (const std::exception& error) {
        logError(kIface, std::string("초기화 실패 - config.json 설정을 확인하세요: ") + error.what());
        return 1;
    } catch (...) {
        logError(kIface, "초기화 실패 - 알 수 없는 예외");
        return 1;
    }

    logSuccess(kIface, "관제 서버 시작 (logLevel=" + config.logLevel + ", dispatcher=" +
                           config.hwHealthCheck.dispatcher + ", channels=" + std::to_string(config.channelCount) + ")");

    // 3) 종료 대기
    //    예전에는 std::cin.get() 으로 기다렸는데, systemd 서비스로 띄우면 stdin 이 없어
    //    즉시 EOF -> 곧바로 종료되어 버렸음 (그리고 SIGTERM 처리도 없었음)
    bool stopRequested = false;
    std::thread signalThread(waitForShutdownSignal, shutdownMask, std::ref(stopRequested));
    signalThread.join();

    controller->stop();
    controller.reset();

    logSuccess(kIface, "관제 서버 정상 종료");
    return 0;
}
