#pragma once

/**
 * @file    Logger.h
 * @brief   compute-server / control-server 공용 로그 (레벨 필터 + 완전 비동기 출력)
 *
 * @details
 * 콘솔 형식: [YYYY-MM-DD-HH:MM:SS] 인터페이스 - Success/Error/Debug: 내용
 * 각 호출부는 자신의 인터페이스 이름(예: "Network", "Parser")을 kIface 등으로 들고 있다가
 * logSuccess/logError/logDebug 첫 인자로 넘기면 됨
 *
 * @note [ 왜 shared/ 인가 ]
 * 예전에는 compute-server/src/log/Logger.h 와 control-server/src/log/Logger.h 에
 * 거의 동일한 사본이 각각 있었음 -- 한쪽만 고치면 두 서버의 로그 동작이 조용히 갈라짐
 * (실제로 compute-server 만 비동기 콘솔로 바꾼 뒤 control-server 는 동기인 채로 남아
 *  "두 서버가 동일한 설계"라던 주석이 거짓이 된 상태였음)
 * -> Contract.h 처럼 한 곳에 두고 두 서버가 함께 빌드되도록 통합
 *
 * @note 프로세스마다 독립적인 싱글턴이므로 서버끼리 큐/파일을 공유하지는 않음
 *       (각 서버가 자기 실행 디렉터리에 자기 CSV를 남김)
 *
 * @note [ 왜 콘솔까지 비동기인가 ]
 * 예전에는 CSV 기록만 비동기였고 콘솔은 호출 스레드에서 직접 std::cout/std::cerr 로 썼음
 * -- 특히 std::cerr 는 unit-buffered 라 logError 한 번마다 write() syscall 이 발생했고,
 *    이게 프레임마다 도는 경로(발행 성공, 팬텀 객체 제거 등)에 그대로 걸려 있었음
 * -- 이제 콘솔/파일 모두 같은 메모리 큐에 넣기만 하고 백그라운드 워커가 배치로 내보냄
 *    -> 파이프라인 스레드에서 로깅 때문에 발생하는 I/O 가 0
 *
 * @warning [ 크래시 시 유실 ]
 * 비동기라서 SIGSEGV 등으로 즉사하면 마지막 flushInterval 구간의 로그가 남지 않음
 * 정상 종료(main 반환 / 시그널 종료) 경로에서는 정적 소멸자가 마지막까지 flush 함
 *
 * @note [ 레벨 ]
 * - Debug   : 프레임마다 도는 정상 이벤트 (발행 성공, 중복 객체 제거, 라우팅 drop)
 * - Info    : 상태 전이/주기 지표 (연결 성공, 5초 주기 성능 리포트, 시작/종료)
 * - Error   : 조치가 필요한 이상
 * 운영 기본값은 Info -- Debug 를 켜면 프레임당 수 건씩 쌓이므로 진단할 때만 사용
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

/// @brief 로그 심각도
enum class LogLevel {
    Debug = 0,
    Info = 1,
    Error = 2,
    Off = 3,  ///< 전부 끔
};

/// @brief 문자열을 LogLevel 로 변환 (알 수 없으면 Info)
inline LogLevel logLevelFromString(std::string_view s) {
    if (s == "debug")
        return LogLevel::Debug;
    if (s == "error")
        return LogLevel::Error;
    if (s == "off")
        return LogLevel::Off;
    return LogLevel::Info;
}

/// @brief 로거 동작 설정 (AppConfig 에서 채워 main 이 initLogger 로 주입)
struct LogConfig {
    LogLevel level = LogLevel::Info;
    bool console = true;            ///< 콘솔 출력 여부 (journald 부하가 부담되면 끄기)
    bool file = true;               ///< CSV 파일 기록 여부
    int flushIntervalMs = 500;      ///< 워커가 큐를 비우는 주기
    int maxPendingEntries = 10000;  ///< 큐 상한 (초과 시 drop-oldest, OOM 방지)

    /**
     * @brief   기록할 CSV 파일 이름 (프로세스의 작업 디렉터리 기준)
     * @details 파일 회전은 logrotate(8) 가 담당하므로 이름은 반드시 '고정'이어야 한다.
     *          예전처럼 날짜별 파일명(YYYY-MM-DD.csv)을 쓰면 logrotate 의 rename/postrotate 와
     *          이중으로 회전해 서로 어긋난다
     */
    std::string fileName = "veda.csv";
};

/// @brief 로깅 구현 세부사항 (외부에서는 logDebug/logSuccess/logError/initLogger 만 쓰면 됨)
namespace log_detail {

/// @brief 현재 유효한 최소 레벨 (핫패스에서 원자적으로 읽음)
inline std::atomic<int> g_minLevel{static_cast<int>(LogLevel::Info)};

/// @brief 한 행에 해당하는 로그 항목
struct LogEntry {
    /**
     * @brief 이벤트 발생 시각 (원시 time_point)
     * @details 호출 스레드는 시각을 '읽기만' 하고(vDSO clock_gettime, ~20ns), 포맷팅은 워커가 한다
     *          -- localtime_r(전역 락) + strftime + 문자열 힙 할당을 핫패스에서 제거하기 위함
     *          ("YYYY-MM-DD-HH:MM:SS" 는 19자라 libstdc++ SSO(15자)를 넘겨 매번 malloc 이 났음)
     */
    std::chrono::system_clock::time_point time;
    const char* iface = "";  ///< 호출부의 정적 문자열이라 복사하지 않고 포인터만 보관
    LogLevel level = LogLevel::Info;
    std::string message;
};

inline constexpr std::string_view toString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Error:
            return "Error";
        case LogLevel::Off:
        case LogLevel::Info:
            break;
    }
    return "Success";
}

/**
 * @brief   콘솔과 CSV 파일에 로그를 비동기·배치로 내보내는 싱글턴
 *
 * @details
 * - enqueue()는 메모리 큐에 push 만 하고 즉시 반환 (I/O 없음, 호출 스레드 지연 없음)
 * - 백그라운드 스레드가 주기적으로 큐 전체를 스왑해서 콘솔+파일에 한 번에 씀
 * - 프로그램 종료 시(정적 소멸자) 남은 로그까지 마지막으로 flush 후 종료
 * - 타임스탬프 포맷팅은 워커가 '초 단위 캐시'로 수행 (배치당 localtime_r/strftime 1~2회)
 *
 * @note [ 파일 회전은 이 클래스가 하지 않음 ]
 * 고정 이름(LogConfig::fileName) 하나에만 append 하고, 회전/압축/보존은 logrotate(8) 에 맡긴다.
 * logrotate 가 create 방식으로 파일을 rename 한 뒤 SIGHUP 을 보내면 requestReopen() 이 호출되고,
 * 워커가 다음 주기에 파일을 닫았다 다시 연다 (copytruncate 의 데이터 유실 구간을 피하기 위함)
 */
class LogSink {
public:
    static LogSink& instance() {
        static LogSink inst;
        return inst;
    }

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    void configure(const LogConfig& config) {
        g_minLevel.store(static_cast<int>(config.level), std::memory_order_relaxed);
        console_.store(config.console, std::memory_order_relaxed);
        file_.store(config.file, std::memory_order_relaxed);
        flushIntervalMs_.store(config.flushIntervalMs > 0 ? config.flushIntervalMs : 1, std::memory_order_relaxed);
        maxPending_.store(config.maxPendingEntries > 0 ? static_cast<std::size_t>(config.maxPendingEntries) : 1,
                          std::memory_order_relaxed);
        if (!config.fileName.empty()) {
            std::lock_guard<std::mutex> lk(fileMutex_);
            fileName_ = config.fileName;
        }
    }

    /**
     * @brief   CSV 를 닫았다 다시 열도록 요청 (logrotate 의 postrotate SIGHUP 에서 호출)
     *
     * @details
     * ofstream 은 워커 스레드 전용이라 여기서 직접 만지지 않고 플래그만 세운다 -> 파일 I/O 에
     * 락을 추가하지 않고도 안전하며, 시그널 감시 스레드는 즉시 반환한다.
     * 실제 close/open 은 워커가 다음 주기에 수행 (진행 중인 배치를 먼저 내보낸 뒤)
     */
    void requestReopen() noexcept {
        reopenRequested_.store(true, std::memory_order_release);
        cv_.notify_one();
    }

    void enqueue(LogEntry entry) {
        bool dropped = false;
        std::uint64_t droppedSnapshot = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pending_.size() >= maxPending_.load(std::memory_order_relaxed)) {
                // drop-oldest: 디스크/콘솔이 막혀 큐가 가득 차면, 오래된 로그보다 최신 로그가
                // 디버깅에 더 유용하다고 보고 가장 오래된 항목을 버림
                pending_.pop_front();
                ++droppedCount_;
                dropped = true;
                droppedSnapshot = droppedCount_;
            }
            pending_.push_back(std::move(entry));
        }
        cv_.notify_one();

        if (dropped && (droppedSnapshot == 1 || droppedSnapshot % 1000 == 0)) {
            // 이 큐에 다시 넣으면 재귀가 되므로 stderr 에 직접 출력
            std::cerr << "[LogSink] 경고: 로그 큐가 가득 차 오래된 로그를 버립니다 (누적 드랍 " << droppedSnapshot
                      << "건)\n";
        }
    }

    ~LogSink() {
        stopping_ = true;
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

private:
    LogSink() { worker_ = std::thread(&LogSink::workerLoop, this); }

    /**
     * @brief   time_point 를 "YYYY-MM-DD-HH:MM:SS" 로 포맷 (초 단위 캐시)
     *
     * @details
     * 한 배치는 flushInterval(기본 500ms) 이내라 대부분의 항목이 같은 '초'를 공유한다.
     * 같은 초면 직전 결과를 그대로 재사용해 localtime_r(glibc 전역 락)/strftime 호출을
     * 배치당 1~2회로 줄인다. assign() 은 기존 capacity 를 재사용하므로 힙 할당도 없다
     *
     * @note    워커 스레드에서만 호출되므로 캐시에 락이 필요 없음
     * @return  내부 버퍼 참조 (다음 호출 전까지 유효 -> 복사 없이 바로 스트림에 씀)
     */
    const std::string& timestampFor(std::chrono::system_clock::time_point tp) {
        const std::time_t sec = std::chrono::system_clock::to_time_t(tp);
        if (tsCacheValid_ && sec == tsCacheSecond_)
            return tsCache_;

        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &sec);
#else
        localtime_r(&sec, &tmBuf);
#endif
        char buf[32];
        const std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H:%M:%S", &tmBuf);
        tsCache_.assign(buf, n);
        tsCacheSecond_ = sec;
        tsCacheValid_ = true;
        return tsCache_;
    }

    /// @brief 현재 CSV 를 flush 후 닫음. 다음 writeFile 이 (logrotate 가 새로 만든) 파일을 연다
    void closeFile() {
        if (ofs_.is_open()) {
            ofs_.flush();
            ofs_.close();
        }
    }

    /// @brief 쉼표/따옴표/개행이 포함된 필드를 CSV 규칙에 맞게 큰따옴표로 감쌈
    static std::string csvEscape(const std::string& field) {
        if (field.find_first_of(",\"\n\r") == std::string::npos)
            return field;

        std::string out = "\"";
        out.reserve(field.size() + 2);
        for (char c : field) {
            if (c == '"')
                out += "\"\"";
            else
                out += c;
        }
        out += "\"";
        return out;
    }

    void writeConsole(const std::deque<LogEntry>& batch) {
        for (const auto& e : batch) {
            std::ostream& out = e.level == LogLevel::Error ? std::cerr : std::cout;
            out << "[" << timestampFor(e.time) << "] " << e.iface << " - " << toString(e.level) << ": " << e.message
                << "\n";
        }
        std::cout.flush();
    }

    /// @brief 고정 이름 파일이 닫혀 있으면 열고, batch 를 한 번에 씀 (회전은 logrotate 담당)
    void writeFile(const std::deque<LogEntry>& batch) {
        if (!ofs_.is_open()) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(fileMutex_);
                path = fileName_;
            }
            ofs_.open(path, std::ios::app | std::ios::ate);
            if (!ofs_.is_open()) {
                // 파일을 못 열어도 콘솔 로그는 이미 나갔으므로 여기서는 조용히 스킵
                return;
            }
            if (ofs_.tellp() == 0)
                ofs_ << "timestamp,interface,level,message\n";
        }

        for (const auto& e : batch) {
            // 타임스탬프는 고정 포맷이라 쉼표/따옴표/개행이 들어갈 수 없음 -> csvEscape 생략
            // (csvEscape 는 값을 복사해 반환하므로, 여기서 부르면 항목마다 불필요한 할당이 생김)
            ofs_ << timestampFor(e.time) << ',' << csvEscape(e.iface) << ',' << std::string(toString(e.level)) << ','
                 << csvEscape(e.message) << '\n';
        }
        ofs_.flush();
    }

    void writeBatch(const std::deque<LogEntry>& batch) {
        if (batch.empty())
            return;
        if (console_.load(std::memory_order_relaxed))
            writeConsole(batch);
        if (file_.load(std::memory_order_relaxed))
            writeFile(batch);
    }

    void workerLoop() {
        while (!stopping_) {
            std::deque<LogEntry> batch;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(flushIntervalMs_.load(std::memory_order_relaxed)),
                             [this] {
                                 return stopping_.load() || !pending_.empty() ||
                                        reopenRequested_.load(std::memory_order_acquire);
                             });
                batch.swap(pending_);
            }
            writeBatch(batch);

            // 배치를 먼저 내보낸 뒤에 닫는다 -> 회전 직전에 쌓인 로그가 유실되지 않음
            // (logrotate 가 rename 한 '옛 파일'에 기록되는 게 맞는 항목들이므로)
            if (reopenRequested_.exchange(false, std::memory_order_acq_rel))
                closeFile();
        }

        // 종료 신호 이후 마지막으로 들어온 항목까지 한 번 더 flush
        std::deque<LogEntry> remaining;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            remaining.swap(pending_);
        }
        writeBatch(remaining);
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<LogEntry> pending_;
    std::uint64_t droppedCount_ = 0;  ///< mtx_ 로 보호됨, drop-oldest 누적 카운트
    std::atomic<bool> stopping_{false};

    std::atomic<bool> console_{true};
    std::atomic<bool> file_{true};
    std::atomic<int> flushIntervalMs_{500};
    std::atomic<std::size_t> maxPending_{10000};

    /// @name 파일 상태 (ofs_ 는 워커 스레드 전용)
    /// @{
    std::mutex fileMutex_;               ///< fileName_ 보호 (configure() vs 워커의 open)
    std::string fileName_ = "veda.csv";  ///< 고정 이름 -- 회전은 logrotate 담당
    std::atomic<bool> reopenRequested_{false};
    std::ofstream ofs_;
    /// @}

    /// @name 타임스탬프 포맷 캐시 (워커 스레드 전용이라 락 없음)
    /// @{
    std::string tsCache_;
    std::time_t tsCacheSecond_ = 0;
    bool tsCacheValid_ = false;
    /// @}
    std::thread worker_;  ///< 마지막에 선언 -> 다른 멤버가 먼저 생성된 뒤 스레드 시작
};

inline void emit(const char* iface, LogLevel level, const std::string& msg) {
    // 핫패스에서는 시각을 '읽기만' 한다 (vDSO clock_gettime, ~20ns).
    // 포맷팅(localtime_r/strftime)과 문자열 할당은 전부 워커 스레드가 수행
    LogSink::instance().enqueue({std::chrono::system_clock::now(), iface, level, msg});
}

}  // namespace log_detail

/**
 * @brief   로거 설정을 적용 (main 이 config 를 읽은 직후 1회 호출)
 * @note    호출 이전의 로그는 기본 설정(Info, 콘솔+파일)으로 처리됨
 */
inline void initLogger(const LogConfig& config) { log_detail::LogSink::instance().configure(config); }

/**
 * @brief   로그 파일을 닫았다 다시 열도록 요청 (logrotate 의 postrotate SIGHUP 핸들러에서 호출)
 * @details 실제 close/open 은 워커 스레드가 다음 주기에 수행하므로 이 호출은 즉시 반환된다.
 *          시그널 감시 스레드(sigwait)에서 호출해도 안전함
 * @warning SIGHUP 은 반드시 '스레드 생성 이전에' 블록해 두어야 한다. 기본 처리 동작이 프로세스
 *          종료라서, 블록하지 않으면 logrotate 가 서버를 죽이게 된다
 */
inline void reopenLogFile() { log_detail::LogSink::instance().requestReopen(); }

/**
 * @brief   해당 레벨이 현재 켜져 있는지
 * @details 메시지 문자열을 만드는 비용 자체가 아까운 자리에서 미리 걸러낼 때 사용
 */
inline bool isLogEnabled(LogLevel level) {
    return static_cast<int>(level) >= log_detail::g_minLevel.load(std::memory_order_relaxed);
}

/// @brief 프레임마다 도는 정상 이벤트 (기본 설정에서는 출력되지 않음)
inline void logDebug(const char* iface, const std::string& msg) {
    if (isLogEnabled(LogLevel::Debug))
        log_detail::emit(iface, LogLevel::Debug, msg);
}

/// @brief 상태 전이/주기 지표 등 평상시에도 남겨야 하는 이벤트
inline void logSuccess(const char* iface, const std::string& msg) {
    if (isLogEnabled(LogLevel::Info))
        log_detail::emit(iface, LogLevel::Info, msg);
}

/// @brief 조치가 필요한 이상
inline void logError(const char* iface, const std::string& msg) {
    if (isLogEnabled(LogLevel::Error))
        log_detail::emit(iface, LogLevel::Error, msg);
}
