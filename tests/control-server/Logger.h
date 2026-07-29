#pragma once

/**
 * @file    Logger.h
 * @brief   control-server 순수 단위 테스트용 무동작 Logger 대역
 *
 * @details 계산 모듈의 프로덕션 .cpp는 Logger.h를 포함하지만 테스트 결과에는 파일 I/O,
 *          백그라운드 스레드, 시간 포맷팅이 필요 없다. 테스트 타깃의 include 경로에서 이
 *          파일을 shared/Logger.h보다 먼저 두어 로깅 런타임 종속성을 제거한다.
 */

#include <string>

// Test-only no-op logger selected through the existing test include path.

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Error = 2,
    Off = 3,
};

inline bool isLogEnabled(LogLevel) noexcept {
    return false;
}

inline void logDebug(const char*, const std::string&) noexcept {}
inline void logSuccess(const char*, const std::string&) noexcept {}
inline void logError(const char*, const std::string&) noexcept {}
