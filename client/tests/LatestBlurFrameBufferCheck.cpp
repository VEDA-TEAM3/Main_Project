#include <cstdio>

#include "network/realtime/LatestBlurFrameBuffer.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

BlurFrameData makeFrame(int channelIndex, qint64 timestamp) {
    BlurFrameData frame;
    frame.channelIndex = channelIndex;
    frame.sourceTimestamp = timestamp;
    return frame;
}
}  // namespace

int main() {
    LatestBlurFrameBuffer buffer;

    check(buffer.submit(makeFrame(0, 100)), "first frame must schedule delivery");
    check(!buffer.submit(makeFrame(0, 200)), "replacement must reuse pending delivery");
    check(!buffer.submit(makeFrame(1, 300)), "another channel must reuse pending delivery");

    const QVector<BlurFrameData> frames = buffer.takeLatestFrames();
    check(frames.size() == 2, "buffer must return at most one frame per channel");
    check(frames.size() > 0 && frames[0].channelIndex == 0 && frames[0].sourceTimestamp == 200,
          "channel zero must keep its latest frame");
    check(frames.size() > 1 && frames[1].channelIndex == 1 && frames[1].sourceTimestamp == 300,
          "channel one must keep its frame");
    check(buffer.takeCoalescedFrameCount() == 1, "replaced frame must be counted");
    check(buffer.submit(makeFrame(0, 400)), "draining must allow a new delivery");

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("LatestBlurFrameBuffer checks passed\n");
    return 0;
}
