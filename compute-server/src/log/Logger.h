#pragma once

/**
 * @file    Logger.h
 * @brief   파이프라인 전 구간 공용 로그 포맷
 *
 * @details
 * 콘솔 형식: [YYYY-MM-DD-HH:MM:SS] 인터페이스 - Success/Error: 내용
 * 각 호출부는 자신의 인터페이스 이름(예: "Network", "Parser")을 kIface 등으로 들고 있다가
 * logSuccess/logError 첫 인자로 넘기면 됨
 *
 * 콘솔 출력과 동시에 같은 내용을 "YYYY-MM-DD.csv" 파일에도 남김 (날짜가 바뀌면 새 파일로 전환)
 * 파일 I/O가 호출 스레드(RTSP 수신 스레드 등)를 막지 않도록, 로그는 우선 메모리 큐에만 쌓고
 * 백그라운드 스레드가 주기적으로 큐 전체를 모아 한 번에 파일에 씀 (배치 기록)
 *
 * @note    이전에는 network/RtspClient.cpp, network/RtspClientV2.cpp,
 *          source/RtspOnvifSource.cpp, source/RtspOnvifSourceV2.cpp 각각에
 *          동일한 코드가 중복 정의되어 있었음 -> 이 헤더로 통일
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// @brief CSV 파일 로깅 구현 세부사항 (외부에서는 logSuccess/logError만 쓰면 됨)
namespace log_detail {

/// @brief CSV 한 행에 해당하는 로그 항목
struct LogEntry {
    std::string timestamp;
    std::string iface;
    std::string level;  ///< "Success" | "Error"
    std::string message;
};

/**
 * @brief   CSV 파일에 로그를 비동기·배치로 기록하는 싱글턴
 *
 * @details
 * - enqueue()는 메모리 큐에 push만 하고 즉시 반환 (디스크 I/O 없음, 호출 스레드 지연 없음)
 * - 백그라운드 스레드가 주기적으로(kFlushInterval) 큐 전체를 한 번에 스왑해서 파일에 씀
 * - 프로그램 종료 시(정적 소멸자) 남은 로그까지 마지막으로 flush 후 종료
 * - 날짜가 바뀌면 자동으로 새 "YYYY-MM-DD.csv" 파일로 전환
 */
class CsvLogSink {
public:
    static CsvLogSink& instance() {
        static CsvLogSink inst;
        return inst;
    }

    void enqueue(LogEntry entry) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_.push_back(std::move(entry));
        }
        cv_.notify_one();
    }

    CsvLogSink(const CsvLogSink&) = delete;
    CsvLogSink& operator=(const CsvLogSink&) = delete;

    ~CsvLogSink() {
        stopping_ = true;
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

private:
    static constexpr std::chrono::milliseconds kFlushInterval{500};

    CsvLogSink() { worker_ = std::thread(&CsvLogSink::workerLoop, this); }

    static std::string currentDateStr() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmBuf);
        return std::string(buf);
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

    /// @brief 필요 시 날짜별 파일로 (재)전환 후 batch를 한 번에 씀
    void writeBatch(const std::vector<LogEntry>& batch) {
        if (batch.empty())
            return;

        const std::string date = currentDateStr();
        if (date != openDate_ || !ofs_.is_open()) {
            if (ofs_.is_open())
                ofs_.close();

            const std::string path = date + ".csv";
            ofs_.open(path, std::ios::app | std::ios::ate);
            if (ofs_.is_open()) {
                openDate_ = date;
                if (ofs_.tellp() == 0)
                    ofs_ << "timestamp,interface,level,message\n";
            } else {
                // 파일을 못 열어도 콘솔 로그는 이미 찍혔으므로 여기서는 조용히 스킵
                return;
            }
        }

        for (const auto& e : batch) {
            ofs_ << csvEscape(e.timestamp) << ',' << csvEscape(e.iface) << ',' << csvEscape(e.level) << ','
                 << csvEscape(e.message) << '\n';
        }
        ofs_.flush();
    }

    void workerLoop() {
        while (!stopping_) {
            std::vector<LogEntry> batch;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, kFlushInterval, [this] { return stopping_.load() || !pending_.empty(); });
                batch.swap(pending_);
            }
            writeBatch(batch);
        }

        // 종료 신호 이후 마지막으로 들어온 항목까지 한 번 더 flush
        std::vector<LogEntry> remaining;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            remaining.swap(pending_);
        }
        writeBatch(remaining);
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<LogEntry> pending_;
    std::atomic<bool> stopping_{false};
    std::string openDate_;
    std::ofstream ofs_;
    std::thread worker_;  ///< 마지막에 선언 -> 다른 멤버가 먼저 생성된 뒤 스레드 시작
};

}  // namespace log_detail

/// @brief 로그 타임스탬프: "YYYY-MM-DD-HH:MM:SS" (로컬 타임)
inline std::string nowTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H:%M:%S", &tmBuf);
    return std::string(buf);
}

/// @brief [DateTime] 인터페이스 - Success: 내용 (콘솔 출력 + CSV 큐잉)
inline void logSuccess(const char* iface, const std::string& msg) {
    const std::string ts = nowTimestamp();
    std::cout << "[" << ts << "] " << iface << " - Success: " << msg << "\n";
    log_detail::CsvLogSink::instance().enqueue({ts, iface, "Success", msg});
}

/// @brief [DateTime] 인터페이스 - Error: 내용 (콘솔 출력 + CSV 큐잉)
inline void logError(const char* iface, const std::string& msg) {
    const std::string ts = nowTimestamp();
    std::cerr << "[" << ts << "] " << iface << " - Error: " << msg << "\n";
    log_detail::CsvLogSink::instance().enqueue({ts, iface, "Error", msg});
}
