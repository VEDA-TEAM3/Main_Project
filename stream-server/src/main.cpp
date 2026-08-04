/**
 * @file    main.cpp
 * @brief   stream-server 진입점
 *
 * @note [ 시그널 처리 — 다른 두 서버와 동일한 규약 ]
 * 스레드를 하나라도 만들기 **전에** SIGINT/SIGTERM/SIGHUP 을 블록한 뒤 전용 sigwait
 * 스레드에서 받는다. 특히 SIGHUP 은 기본 처분이 '프로세스 종료' 라서, 블록하지 않으면
 * logrotate 가 회전할 때마다 서버가 죽는다. 여기서는 SIGHUP 을 '로그 파일 재오픈'
 * 으로 해석하고 계속 돈다.
 */

#include <gst/gst.h>
#include <pthread.h>  // pthread_sigmask — 스레드 단위 마스크여야 한다 (sigprocmask 아님)

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "Logger.h"
#include "core/AppContext.h"

namespace {

constexpr const char* kIface = "main";
constexpr const char* kConfigPath = "config.json";

/**
 * @brief 종료 시그널이 올 때까지 블록한다 (SIGHUP 은 로그 재오픈 후 계속 대기)
 * @note  control-server 는 전용 스레드에서 sigwait 하지만, 여기서는 GMainLoop 이
 *        AppContext 의 배경 스레드에 있어 main 스레드가 놀고 있다 -- 스레드를 하나 더
 *        만들 이유가 없다.
 */
void waitForShutdown(const sigset_t& blocked) {
    while (true) {
        int received = 0;
        if (sigwait(&blocked, &received) != 0) {
            return;  // sigwait 실패는 회복 대상이 아니다
        }
        if (received == SIGHUP) {
            // logrotate 의 postrotate 가 보낸다. 죽지 않고 파일만 다시 연다.
            reopenLogFile();
            continue;
        }
        logSuccess(kIface, "종료 시그널 수신 (" + std::to_string(received) + ")");
        return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // [1] 시그널 블록 — 반드시 어떤 스레드보다 먼저.
    // gst_init 도 내부적으로 스레드를 만들 수 있으므로 그보다도 앞이다.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) {
        std::cerr << "[main] 시그널 마스크 설정 실패 — 종료합니다.\n";
        return EXIT_FAILURE;
    }

    // [2] GStreamer 초기화. 인자를 넘겨 GST_DEBUG 류 옵션을 지원한다.
    gst_init(&argc, &argv);

    // [3] 설정 로드 (예외를 던지지 않음 — 실패해도 기본값으로 진행)
    const AppConfig config = AppConfig::load(kConfigPath);

    LogConfig logConfig;
    logConfig.level = logLevelFromString(config.logLevel);
    logConfig.console = config.logToConsole;
    logConfig.file = config.logToFile;
    logConfig.fileName = config.logFileName;
    logConfig.flushIntervalMs = config.logFlushIntervalMs;
    logConfig.maxPendingEntries = config.logMaxPendingEntries;
    initLogger(logConfig);

    int exitCode = EXIT_SUCCESS;
    try {
        // [4] 조립 및 기동. 조립 시점 오류(잘못된 URI/경로)는 생성자가 던지고 여기서 잡는다
        //     -- 조용히 틀린 파이프라인으로 도는 것보다 죽는 편이 낫다.
        AppContext context(config);
        if (!context.start()) {
            logError(kIface, "기동 실패 — 설정을 확인하세요");
            exitCode = EXIT_FAILURE;
        } else {
            // [5] 종료 대기. GMainLoop 은 AppContext 가 배경 스레드에서 돌린다.
            waitForShutdown(blocked);
        }
        // context 소멸자가 stop() 을 부른다 (멱등)
    } catch (const std::exception& e) {
        logError(kIface, std::string("치명적 오류: ") + e.what());
        exitCode = EXIT_FAILURE;
    }

    // 로거는 명시적 종료 함수가 없다 -- LogSink 싱글턴 소멸자가 남은 큐를 비운다.
    // gst_deinit() 은 그 이후에 부른다 (GStreamer 를 먼저 내리면 로그 경로가 죽는다).
    gst_deinit();
    return exitCode;
}
