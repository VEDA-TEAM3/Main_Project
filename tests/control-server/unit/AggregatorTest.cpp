/**
 * @file    AggregatorTest.cpp
 * @brief   TimeWindowAggregatorV2 / FrameBufferPool 격리 단위 테스트 (무할당 리팩터 회귀 방지)
 *
 * @details
 * 감사에서 확인한 항목을 고정한다:
 *  - [A1] kMaxObjectsPerFrame(256) 초과 프레임 통째로 드롭 (OOM 방어)
 *  - [A2] channelCount 범위/ null clock 을 생성자에서 거부 (거대 할당 방어)
 *  - [A3] 핫패스 무할당 -- 슬롯 버퍼 swap 회전 + 풀 대여/반납
 *  - [A5] channelId 범위 검사 (인덱싱 전)
 *  - 콜백이 const& 로 '빌린' 버퍼를 받는다는 수명 계약
 *  - FrameBufferPool: release() 가 clear() 하지 않음, acquire() fail-open
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "Logger.h"
#include "aggregate/FrameBufferPool.h"
#include "aggregate/TimeWindowAggregatorV2.h"

namespace {

long g_allocCount = 0;
bool g_allocCounting = false;

/// @brief 시간을 수동으로 전진시키는 시계 (윈도우 마감 시점을 정확히 제어)
class FakeClock final : public IClock {
public:
    veda::TimestampMs now() const override { return t_; }
    void advance(veda::TimestampMs ms) { t_ += ms; }

private:
    veda::TimestampMs t_ = 1000;
};

veda::TopViewFrame makeFrame(veda::ChannelId ch, std::size_t objectCount = 3) {
    veda::TopViewFrame f;
    f.v = veda::kSchemaVersion;
    f.ts = 1000;
    f.ch = ch;
    f.objects.resize(objectCount);
    for (std::size_t i = 0; i < objectCount; ++i) {
        f.objects[i].id = static_cast<veda::ObjectId>(i + 1);
        f.objects[i].cls = veda::ObjectClass::Human;
        f.objects[i].pos.x = static_cast<double>(i);
        f.objects[i].pos.y = static_cast<double>(i) * 2.0;
    }
    return f;
}

/// @brief 콜백이 받은 묶음을 '값으로' 기록 (참조를 보관하면 안 되므로 복사한다)
struct Recorder {
    int callCount = 0;
    std::vector<veda::TopViewFrame> lastCopy;
    const void* lastAddress = nullptr;

    IFrameAggregator::AggregationCallback callback() {
        return [this](const IFrameAggregator::AggregatedFrames& frames) {
            ++callCount;
            lastAddress = static_cast<const void*>(frames.data());
            lastCopy = frames;  // 계약대로 '복사'해서 보관
        };
    }
};

/// @brief 로거를 끈 상태로 고정 (테스트가 파일/콘솔 I/O 를 일으키지 않도록)
struct LoggerOff {
    LoggerOff() {
        LogConfig c;
        c.level = LogLevel::Off;
        c.console = false;
        c.file = false;
        initLogger(c);
    }
};
const LoggerOff g_loggerOff;

}  // namespace

void* operator new(std::size_t n) {
    if (g_allocCounting) ++g_allocCount;
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ============================================================================
// 1. 생성자 검증 (거대 할당 / 널 역참조 방어)
// ============================================================================

TEST(AggregatorTest, RejectsNullClock) {
    // clock_->now() 는 mutex_ 를 쥔 채 호출된다. null 이면 락을 쥔 상태로 죽어
    // 다른 스레드까지 함께 멈춘다.
    EXPECT_THROW(TimeWindowAggregatorV2(nullptr, 100, 4), std::invalid_argument);
}

TEST(AggregatorTest, RejectsNegativeChannelCount) {
    // -1 이 size_t 로 캐스팅되면 18446744073709551615 -> 생성자가 즉시 수십 EB 를 요구한다.
    auto clock = std::make_shared<FakeClock>();
    EXPECT_THROW(TimeWindowAggregatorV2(clock, 100, -1), std::invalid_argument);
}

TEST(AggregatorTest, RejectsZeroChannelCount) {
    auto clock = std::make_shared<FakeClock>();
    EXPECT_THROW(TimeWindowAggregatorV2(clock, 100, 0), std::invalid_argument);
}

TEST(AggregatorTest, RejectsChannelCountAboveHardwareLimit) {
    // 하드웨어 채널 ID 가 uint8_t 범위이므로 상한은 256 이다.
    auto clock = std::make_shared<FakeClock>();
    EXPECT_THROW(TimeWindowAggregatorV2(clock, 100, 257), std::invalid_argument);
    EXPECT_NO_THROW(TimeWindowAggregatorV2(clock, 100, 256));
}

// ============================================================================
// 2. 경계 검사 (channelId / kMaxObjectsPerFrame)
// ============================================================================

TEST(AggregatorTest, DropsFrameWithChannelIdOutOfRange) {
    // slots_[ch] 인덱싱 '전에' 검사하므로 힙 오염이 성립할 경로가 없다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(4));   // 상한 밖 (channelCount == 4 이므로 유효 범위는 0..3)
    agg.push(makeFrame(99));
    clock->advance(200);
    agg.push(makeFrame(0));  // 이 push 가 이전 윈도우를 마감시킨다

    EXPECT_EQ(rec.callCount, 0) << "범위 밖 프레임만 있었으므로 마감할 데이터가 없어야 한다";
}

TEST(AggregatorTest, DropsFrameWithNegativeChannelId) {
    // ChannelId 는 부호 있는 타입이다. 하한 검사가 빠지면 size_t 캐스팅으로 거대 인덱스가 된다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(-1));
    clock->advance(200);
    agg.push(makeFrame(0));

    EXPECT_EQ(rec.callCount, 0);
}

TEST(AggregatorTest, DropsFrameExceedingMaxObjectsPerFrame) {
    // [A1] 원격 compute-server 가 보낸 객체 수는 신뢰할 수 없다.
    // 상한이 없으면 채널당 수십 MB 가 슬롯에 상주해 256M 컨테이너를 OOMKill 한다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(0, TimeWindowAggregatorV2::kMaxObjectsPerFrame + 1));
    clock->advance(200);
    agg.push(makeFrame(1, 1));

    EXPECT_EQ(rec.callCount, 0) << "상한 초과 프레임은 통째로 드롭되어야 한다";
}

TEST(AggregatorTest, AcceptsFrameExactlyAtMaxObjectsPerFrame) {
    // 경계는 '초과'만 거부한다 (> 이지 >= 가 아니다).
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(0, TimeWindowAggregatorV2::kMaxObjectsPerFrame));
    clock->advance(200);
    agg.push(makeFrame(1, 1));

    ASSERT_EQ(rec.callCount, 1);
    ASSERT_EQ(rec.lastCopy.size(), 1u);
    EXPECT_EQ(rec.lastCopy[0].objects.size(), TimeWindowAggregatorV2::kMaxObjectsPerFrame);
}

TEST(AggregatorTest, PartialFrameIsNeverDelivered) {
    // 잘라서 받으면 '일부만 반영된 프레임'이 되어 융합/위험 판정이 조용히 틀린 답을 낸다.
    // 드롭이 명시적으로 더 안전하다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(0, TimeWindowAggregatorV2::kMaxObjectsPerFrame + 500));
    agg.push(makeFrame(1, 2));  // 정상 채널
    clock->advance(200);
    agg.push(makeFrame(2, 1));

    ASSERT_EQ(rec.callCount, 1);
    ASSERT_EQ(rec.lastCopy.size(), 1u) << "정상 채널 1개만 전달되어야 한다";
    EXPECT_EQ(rec.lastCopy[0].ch, 1);
    EXPECT_EQ(rec.lastCopy[0].objects.size(), 2u);
}

// ============================================================================
// 3. 윈도우 집계 정책
// ============================================================================

TEST(AggregatorTest, NoCallbackBeforeWindowElapses) {
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(0));
    agg.push(makeFrame(1));
    clock->advance(50);  // 윈도우(100ms) 미달
    agg.push(makeFrame(2));

    EXPECT_EQ(rec.callCount, 0);
}

TEST(AggregatorTest, KeepsOnlyLatestFramePerChannel) {
    // TopViewFrame 은 델타가 아니라 전체 상태 스냅샷이다.
    // 같은 채널의 프레임 N+1 은 N 을 완전히 대체한다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    auto first = makeFrame(0, 1);
    first.ts = 1111;
    auto second = makeFrame(0, 5);
    second.ts = 2222;

    agg.push(first);
    agg.push(second);
    clock->advance(200);
    agg.push(makeFrame(1, 1));

    ASSERT_EQ(rec.callCount, 1);
    ASSERT_EQ(rec.lastCopy.size(), 1u) << "같은 채널은 하나로 합쳐져야 한다";
    EXPECT_EQ(rec.lastCopy[0].ts, 2222) << "최신 프레임이 남아야 한다";
    EXPECT_EQ(rec.lastCopy[0].objects.size(), 5u);
}

TEST(AggregatorTest, DeliversAllFilledChannelsOnWindowClose) {
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    agg.push(makeFrame(0));
    agg.push(makeFrame(1));
    agg.push(makeFrame(2));
    clock->advance(200);
    agg.push(makeFrame(3));

    ASSERT_EQ(rec.callCount, 1);
    EXPECT_EQ(rec.lastCopy.size(), 3u) << "마감 시점에 채워져 있던 3채널이 전달되어야 한다";
}

TEST(AggregatorTest, SurvivesWindowCloseWithoutCallbackRegistered) {
    // 콜백 미등록 상태로 마감하면 데이터는 버려지되 크래시하지 않아야 한다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);

    agg.push(makeFrame(0));
    clock->advance(200);
    EXPECT_NO_THROW(agg.push(makeFrame(1)));
}

// ============================================================================
// 4. 콜백 수명 계약 (const& 로 빌려주는 버퍼)
// ============================================================================

TEST(AggregatorTest, CallbackReceivesBorrowedBufferThatIsReusedNextWindow) {
    // [ 계약 ] 콜백이 받는 참조는 반환 즉시 풀로 회수되어 다음 윈도우에 덮어써진다.
    // 같은 버퍼가 재사용되는지를 data() 주소로 확인한다 -- 매 윈도우 새 벡터를 만들면
    // 주소가 계속 바뀌고, 그것은 무할당 성질이 깨졌다는 뜻이다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    Recorder rec;
    agg.setCallback(rec.callback());

    std::vector<const void*> addresses;
    for (int w = 0; w < 5; ++w) {
        agg.push(makeFrame(0));
        agg.push(makeFrame(1));
        clock->advance(200);
        agg.push(makeFrame(2));
        addresses.push_back(rec.lastAddress);
    }

    ASSERT_EQ(rec.callCount, 5);
    // warmup(첫 윈도우) 이후에는 같은 풀 버퍼가 계속 돌아와야 한다.
    for (std::size_t i = 2; i < addresses.size(); ++i) {
        EXPECT_EQ(addresses[i], addresses[1]) << "윈도우 " << i << " 에서 새 버퍼가 만들어졌다 (풀 회수 실패)";
    }
}

TEST(AggregatorTest, CopyingInsideCallbackIsSafeAcrossWindows) {
    // 계약을 지킨 소비자(복사해서 보관)는 다음 윈도우가 버퍼를 덮어써도 영향받지 않는다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);

    std::vector<veda::TopViewFrame> saved;
    agg.setCallback([&saved](const IFrameAggregator::AggregatedFrames& frames) { saved = frames; });

    auto f = makeFrame(0, 2);
    f.ts = 4242;
    agg.push(f);
    clock->advance(200);
    agg.push(makeFrame(1, 1));

    ASSERT_EQ(saved.size(), 1u);
    const auto tsAfterFirst = saved[0].ts;

    // 다음 윈도우 -- 같은 버퍼가 재사용되지만 saved 는 복사본이라 무사해야 한다.
    agg.push(makeFrame(2, 7));
    clock->advance(200);
    agg.push(makeFrame(3, 1));

    EXPECT_EQ(tsAfterFirst, 4242);
}

// ============================================================================
// 5. 무할당 (슬롯 swap 회전 + 풀 대여/반납)
// ============================================================================

TEST(AggregatorTest, SteadyStatePushAndFlushDoNotAllocate) {
    // [A3] 리팩터 전에는 윈도우당 13회(채널 12 + 외곽 벡터 1) 할당했다.
    // optional::reset() 이 objects 버퍼를 해제해 다음 push 가 반드시 재할당했기 때문이다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);

    // 콜백은 아무것도 보관하지 않는다 (복사하면 그 할당이 계측에 섞인다).
    std::size_t seen = 0;
    agg.setCallback([&seen](const IFrameAggregator::AggregatedFrames& frames) { seen += frames.size(); });

    const auto f0 = makeFrame(0, 8);
    const auto f1 = makeFrame(1, 8);
    const auto f2 = makeFrame(2, 8);
    const auto f3 = makeFrame(3, 8);

    // warmup: 슬롯/풀 버퍼의 capacity 를 안정화시킨다.
    for (int w = 0; w < 50; ++w) {
        agg.push(f0);
        agg.push(f1);
        agg.push(f2);
        clock->advance(200);
        agg.push(f3);
    }

    g_allocCount = 0;
    g_allocCounting = true;
    for (int w = 0; w < 200; ++w) {
        agg.push(f0);
        agg.push(f1);
        agg.push(f2);
        clock->advance(200);
        agg.push(f3);
    }
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "정상상태 핫패스에서 힙 할당이 발생했다 "
                                  "(슬롯 swap 회전 또는 FrameBufferPool 반납이 깨졌을 가능성)";
    EXPECT_GT(seen, 0u);
}

TEST(AggregatorTest, SlotBufferCapacitySurvivesWindowClose) {
    // swap 회전의 핵심: 마감 후에도 슬롯이 capacity 를 유지해 다음 push 가 재할당하지 않는다.
    // 큰 프레임 -> 마감 -> 같은 크기 프레임 순서에서 할당이 0이어야 한다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 2);
    agg.setCallback([](const IFrameAggregator::AggregatedFrames&) {});

    const auto big = makeFrame(0, 200);
    const auto other = makeFrame(1, 1);

    for (int w = 0; w < 20; ++w) {  // warmup
        agg.push(big);
        clock->advance(200);
        agg.push(other);
    }

    g_allocCount = 0;
    g_allocCounting = true;
    for (int w = 0; w < 100; ++w) {
        agg.push(big);
        clock->advance(200);
        agg.push(other);
    }
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0) << "마감 시 슬롯 버퍼가 해제되면(reset 계열) 다음 push 가 재할당한다";
}

// ============================================================================
// 6. FrameBufferPool 단독 계약
// ============================================================================

TEST(AggregatorTest, PoolReleaseDoesNotClearElements) {
    // [ 핵심 ] release() 가 clear() 하면 TopViewFrame 원소가 파괴되면서
    // 그 안의 objects 버퍼까지 해제되어 풀의 존재 이유가 사라진다.
    FrameBufferPool pool(2, 4);

    auto buf = pool.acquire();
    buf.resize(2);
    buf[0].objects.resize(64);
    buf[1].objects.resize(64);
    const auto cap0 = buf[0].objects.capacity();
    ASSERT_GT(cap0, 0u);

    pool.release(std::move(buf));

    auto again = pool.acquire();
    ASSERT_GE(again.size(), 2u) << "원소가 파괴되어 있으면 안 된다";
    EXPECT_GE(again[0].objects.capacity(), cap0) << "objects 버퍼 capacity 가 유지되어야 한다";
}

TEST(AggregatorTest, PoolReusesBuffersWithoutAllocating) {
    FrameBufferPool pool(2, 4);
    {
        auto warm = pool.acquire();
        warm.resize(4);
        pool.release(std::move(warm));
    }

    g_allocCount = 0;
    g_allocCounting = true;
    for (int i = 0; i < 100; ++i) {
        auto b = pool.acquire();
        b.resize(4);
        pool.release(std::move(b));
    }
    g_allocCounting = false;

    EXPECT_EQ(g_allocCount, 0);
}

TEST(AggregatorTest, PoolIsFailOpenWhenExhausted) {
    // 실시간 경로에서 "버퍼가 없어 프레임을 버림"은 할당 1회보다 나쁜 결과다.
    // 고갈 시 새로 만들되 그 사실을 계수한다.
    FrameBufferPool pool(1, 4);

    auto a = pool.acquire();
    EXPECT_EQ(pool.exhaustedCount(), 0u);

    auto b = pool.acquire();  // 풀이 비었다
    EXPECT_EQ(pool.exhaustedCount(), 1u) << "고갈이 계수되어야 kFlushBufferPoolSize 부족을 진단할 수 있다";

    b.resize(1);  // 반환된 버퍼가 실제로 쓸 수 있는 상태인지 확인
    EXPECT_EQ(b.size(), 1u);

    pool.release(std::move(a));
    pool.release(std::move(b));
}

TEST(AggregatorTest, PoolExhaustionCountStaysZeroInSteadyState) {
    // 실배포는 push() 가 단일 스레드라 동시에 살아 있는 마감 버퍼가 1개뿐이다.
    auto clock = std::make_shared<FakeClock>();
    TimeWindowAggregatorV2 agg(clock, 100, 4);
    agg.setCallback([](const IFrameAggregator::AggregatedFrames&) {});

    for (int w = 0; w < 100; ++w) {
        agg.push(makeFrame(0));
        agg.push(makeFrame(1));
        clock->advance(200);
        agg.push(makeFrame(2));
    }
    SUCCEED() << "단일 스레드 정상 운용에서 풀 고갈 없이 완주";
}
