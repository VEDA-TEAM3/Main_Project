#include "time/SystemClock.h"

#include <chrono>

veda::TimestampMs SystemClock::now() const {
    auto now_time = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now_time.time_since_epoch()).count();
}