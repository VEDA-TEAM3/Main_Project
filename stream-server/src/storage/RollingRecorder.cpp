#include "storage/RollingRecorder.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "Logger.h"

namespace fs = std::filesystem;

namespace {
constexpr const char* kIface = "RollingRecorder";
constexpr const char* kSegmentExtension = ".mkv";
}  // namespace

RollingRecorder::RollingRecorder(const ChannelConfig& channel, GstElement* splitMuxSink)
    : channel_(channel), splitMuxSink_(splitMuxSink) {
    if (splitMuxSink_ == nullptr) {
        throw std::invalid_argument("RollingRecorder: splitMuxSink must not be null");
    }
    if (!isSafeDirectory(channel_.segmentDir)) {
        // 경로 조작을 여기서 한 번 더 막는다. 이 값은 곧 파일 생성/삭제 경로가 된다.
        throw std::invalid_argument("RollingRecorder: unsafe segmentDir");
    }

    std::error_code ec;
    fs::create_directories(channel_.segmentDir, ec);
    if (ec) {
        throw std::invalid_argument("RollingRecorder: cannot create segmentDir: " + ec.message());
    }

    // format-location: 다음 세그먼트 파일 경로를 우리가 정한다.
    // splitmuxsink 의 location 프로퍼티에 %05d 를 넣는 방식도 있지만, 그러면 파일명이
    // 파이프라인 문자열/프로퍼티로 흘러가고 우리가 목록을 못 잡는다. 시그널로 받으면
    // 생성 시점에 바로 목록에 넣을 수 있어 보존 정책이 정확해진다.
    g_signal_connect(splitMuxSink_, "format-location", G_CALLBACK(&RollingRecorder::onFormatLocation), this);

    // 삭제는 blocking I/O 다. 스트리밍 스레드에서 하면 tee 가 막히고 중계까지 멈춘다.
    // 전용 워커에 넘기고, 워커는 할 일이 없으면 잠든다.
    retentionWorker_ = std::thread(&RollingRecorder::retentionLoop, this);
}

RollingRecorder::~RollingRecorder() {
    {
        std::lock_guard<std::mutex> lock(workMutex_);
        stopping_ = true;
    }
    workCv_.notify_all();  // notify 는 반드시 같은 뮤텍스를 거친 뒤에 (lost wakeup 방지)
    if (retentionWorker_.joinable()) {
        retentionWorker_.join();
    }
}

void RollingRecorder::setSegmentClosedCallback(SegmentClosedCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    segmentClosedCallback_ = std::move(callback);
}

std::string RollingRecorder::currentSegment() const {
    std::lock_guard<std::mutex> lock(segmentMutex_);
    return currentSegment_;
}

void RollingRecorder::scanExistingSegments() {
    // 재시작해도 상한이 유지되어야 한다. 스캔하지 않으면 이전 실행이 남긴 파일이
    // 회계에서 빠져 디스크가 조용히 가득 찬다.
    std::vector<Segment> found;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(channel_.segmentDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != kSegmentExtension) {
            continue;
        }
        const auto size = entry.file_size(ec);
        if (ec) {
            continue;
        }
        found.push_back(Segment{entry.path().string(), static_cast<std::uint64_t>(size)});
    }

    // 파일명이 seg_%05d.mkv 라 사전순 = 생성순이다 (5자리 제로패딩이 그걸 보장한다).
    std::sort(found.begin(), found.end(),
              [](const Segment& a, const Segment& b) { return a.path < b.path; });

    std::uint64_t total = 0;
    for (const Segment& segment : found) {
        total += segment.bytes;
    }

    {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        segments_.assign(found.begin(), found.end());
    }
    usedBytes_.store(total, std::memory_order_relaxed);

    logSuccess(kIface, "ch=" + std::to_string(channel_.channelId) + " 기존 세그먼트 " +
                           std::to_string(found.size()) + "개, " + std::to_string(total / (1024 * 1024)) + " MiB");
    requestRetention();
}

gchar* RollingRecorder::onFormatLocation(GstElement* /*splitmux*/, guint fragmentId, gpointer userData) noexcept {
    auto* self = static_cast<RollingRecorder*>(userData);

    // [보안] snprintf 로 경계를 고정한다. segmentDir 은 이미 검증됐고 길이도 제한(512)되어
    // 있으므로 버퍼가 넘칠 수 없지만, 상한을 코드로 못 박아 두면 나중에 누가 경로 길이
    // 제한을 풀어도 여기서 잘린다.
    char path[kMaxPathLength + 32];
    const int written = std::snprintf(path, sizeof(path), "%s/seg_%05u%s", self->channel_.segmentDir.c_str(),
                                      static_cast<unsigned>(fragmentId), kSegmentExtension);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
        logError(kIface, "세그먼트 경로 조립 실패 (경로가 너무 김)");
        return nullptr;  // splitmuxsink 가 기본 location 으로 대체한다
    }

    // fragmentId N 을 여는 시점 = N-1 이 방금 닫힌 시점. splitmuxsink 에 '세그먼트 종료'
    // 시그널이 따로 없으므로 이 전이를 그대로 쓴다 (추가 API 불필요).
    self->onSegmentOpened(path);
    return g_strdup(path);  // splitmuxsink 가 g_free 한다
}

void RollingRecorder::onSegmentOpened(const char* path) noexcept {
    std::string closed;
    {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        closed = currentSegment_;
        currentSegment_ = path;
    }

    if (!closed.empty()) {
        std::error_code ec;
        const auto size = fs::file_size(closed, ec);
        const std::uint64_t bytes = ec ? 0U : static_cast<std::uint64_t>(size);

        {
            std::lock_guard<std::mutex> lock(segmentMutex_);
            segments_.push_back(Segment{closed, bytes});
        }
        usedBytes_.fetch_add(bytes, std::memory_order_relaxed);

        SegmentClosedCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = segmentClosedCallback_;
        }
        if (callback) {
            callback(closed, bytes);
        }
    }

    requestRetention();  // 실제 삭제는 워커가 한다 -- 이 스레드는 스트리밍 스레드다
}

void RollingRecorder::requestRetention() noexcept {
    {
        std::lock_guard<std::mutex> lock(workMutex_);
        retentionPending_ = true;
    }
    workCv_.notify_one();
}

void RollingRecorder::retentionLoop() noexcept {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(workMutex_);
            workCv_.wait(lock, [this] { return stopping_ || retentionPending_; });
            if (stopping_) {
                return;
            }
            retentionPending_ = false;
        }
        enforceRetention();
    }
}

void RollingRecorder::enforceRetention() {
    // 두 상한 중 **먼저 걸리는 쪽**으로 오래된 것부터 지운다.
    // 개수만 보면(splitmuxsink 의 max-files) 가변 비트레이트 카메라에서 사용량이 몇 배까지
    // 흔들리고, 용량만 보면 파일이 무한히 쌓여 디렉터리 열거가 느려진다.
    while (true) {
        Segment victim;
        {
            std::lock_guard<std::mutex> lock(segmentMutex_);
            const bool overBytes = usedBytes_.load(std::memory_order_relaxed) > channel_.retentionBytes;
            const bool overCount = segments_.size() > channel_.retentionSegments;
            if (segments_.empty() || (!overBytes && !overCount)) {
                return;
            }
            victim = segments_.front();
            segments_.pop_front();
        }

        std::error_code ec;
        if (!fs::remove(victim.path, ec) || ec) {
            logError(kIface, "세그먼트 삭제 실패: " + victim.path + " (" + ec.message() + ")");
            // 삭제에 실패한 파일을 목록에 되돌리지 않는다. 되돌리면 같은 파일을 무한히
            // 재시도하면서 그 뒤의 파일을 못 지우고, 결국 디스크가 찬다.
        }

        // fetch_sub 는 언더플로가 나면 거대한 값이 된다. 회계가 어긋났을 때
        // '상한 초과' 로 오판해 멀쩡한 파일을 계속 지우는 쪽이 더 나쁘다.
        std::uint64_t expected = usedBytes_.load(std::memory_order_relaxed);
        while (!usedBytes_.compare_exchange_weak(expected, expected > victim.bytes ? expected - victim.bytes : 0U,
                                                 std::memory_order_relaxed)) {
        }
    }
}
