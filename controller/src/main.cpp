/**
 * @file    main.cpp
 * @brief   관제 서버 진입점
 */
#include <condition_variable>
#include <csignal>
#include <mutex>

#include "core/AppConfig.h"
#include "core/AppContext.h"

namespace {

std::mutex g_shutdownMutex;
std::condition_variable g_shutdownCv;
bool g_shutdownRequested = false;

/**
 * @brief   SIGINT/SIGTERM 핸들러. 종료 요청 플래그를 세우고 대기 중인 메인 스레드를 깨운다.
 * @note    시그널 핸들러 내부이므로 async-signal-safe한 동작만 수행한다
 *          (mutex/condition_variable 알림은 표준적으로 허용되는 범위 내에서 사용).
 */
void handleShutdownSignal(int /*signal*/) {
    {
        std::lock_guard<std::mutex> lock(g_shutdownMutex);
        g_shutdownRequested = true;
    }
    g_shutdownCv.notify_all();
}

}  // namespace

int main() {
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);

    AppContext context(AppConfig::fromEnv());
    context.run();

    {
        std::unique_lock<std::mutex> lock(g_shutdownMutex);
        g_shutdownCv.wait(lock, [] { return g_shutdownRequested; });
    }

    context.shutdown();
    return 0;
}