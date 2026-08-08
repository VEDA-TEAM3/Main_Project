#include <gst/gst.h>

#include <cstdio>
#include <optional>

#include "video/VideoUtcClockMapper.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

GstBuffer* bufferAt(qint64 ptsMsec, std::optional<qint64> utcMsec = std::nullopt) {
    GstBuffer* buffer = gst_buffer_new();
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(ptsMsec) * GST_MSECOND;

    if (utcMsec.has_value()) {
        GstCaps* reference = gst_caps_new_empty_simple("timestamp/x-unix");
        gst_buffer_add_reference_timestamp_meta(buffer, reference, static_cast<GstClockTime>(*utcMsec) * GST_MSECOND,
                                                GST_CLOCK_TIME_NONE);
        gst_caps_unref(reference);
    }
    return buffer;
}

void checkReferenceTimestampOutlierIsRejected() {
    constexpr qint64 baseUtcMsec = 1786000000000LL;
    VideoUtcClockMapper mapper;

    GstBuffer* initial = bufferAt(1000, baseUtcMsec);
    const std::optional<VideoUtcTimestamp> initialTimestamp = mapper.timestampFor(initial);
    gst_buffer_unref(initial);
    check(initialTimestamp.has_value() && initialTimestamp->utcMsec == baseUtcMsec,
          "the first sender timestamp must establish the UTC anchor");

    GstBuffer* outlier = bufferAt(1200, baseUtcMsec + 10000);
    const std::optional<VideoUtcTimestamp> outlierTimestamp = mapper.timestampFor(outlier);
    gst_buffer_unref(outlier);
    check(outlierTimestamp.has_value() && outlierTimestamp->utcMsec == baseUtcMsec + 200,
          "a discontinuous sender timestamp must use the existing PTS anchor");

    GstBuffer* following = bufferAt(1300);
    const std::optional<VideoUtcTimestamp> followingTimestamp = mapper.timestampFor(following);
    gst_buffer_unref(following);
    check(followingTimestamp.has_value() && followingTimestamp->utcMsec == baseUtcMsec + 300,
          "a rejected outlier must not corrupt following PTS conversion");
}
}  // namespace

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    checkReferenceTimestampOutlierIsRejected();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("VideoUtcClockMapper checks passed\n");
    return 0;
}
