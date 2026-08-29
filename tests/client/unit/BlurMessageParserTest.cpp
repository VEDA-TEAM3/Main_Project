#include <QByteArray>
#include <QString>
#include <cstdio>

#include "network/parsing/BlurMessageParser.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failureCount;
    }
}

bool parse(const QByteArray& payload, int topicChannel, BlurFrameData& frame) {
    QString error;
    return BlurMessageParser::parse(payload, QStringLiteral("veda/ch/%1/blur").arg(topicChannel), topicChannel, frame,
                                    error, 4);
}

void checkValidPrivacyTargets() {
    BlurFrameData frame;
    check(parse(R"({"v":1,"ts":1787000000123,"ch":2,"blurs":[
        {"id":1,"cls":"Head","box":{"l":0.1,"t":0.2,"r":0.3,"b":0.4}},
        {"id":2,"cls":"LicensePlate","box":{"l":-0.1,"t":0.3,"r":0.4,"b":1.1}}
    ]})",
                2, frame),
          "valid blur metadata must parse");
    check(frame.channelIndex == 2, "topic channel must be preserved");
    check(frame.sourceTimestamp == 1787000000123LL, "CCTV UTC timestamp must be preserved");
    check(frame.regions.size() == 2, "all valid privacy targets must be retained");
    check(frame.regions.value(1).normalizedBox.left() == 0.0, "boxes must be clipped to the video bounds");
    check(frame.regions.value(1).normalizedBox.bottom() == 1.0, "boxes must be clipped to the video bounds");
}

void checkMalformedMessagesFailClosed() {
    BlurFrameData frame;
    check(!parse("{broken", 0, frame), "malformed JSON must be rejected");
    check(!parse(R"({"v":2,"ts":1,"ch":0,"blurs":[]})", 0, frame), "unknown schema must be rejected");
    check(!parse(R"({"v":1,"ts":1,"ch":1,"blurs":[]})", 0, frame), "topic and payload channels must match");
    check(!parse(R"({"v":1,"ts":1,"ch":0,"blurs":[
        {"id":1,"cls":"Human","box":{"l":0.1,"t":0.2,"r":0.3,"b":0.4}}
    ]})",
                 0, frame),
          "non-privacy classes must be rejected");
    check(!parse(R"({"v":1,"ts":1,"ch":0,"blurs":[
        {"id":1,"cls":"Head","box":{"l":0.3,"t":0.2,"r":0.1,"b":0.4}}
    ]})",
                 0, frame),
          "inverted boxes must be rejected");
}
}  // namespace

int main() {
    checkValidPrivacyTargets();
    checkMalformedMessagesFailClosed();

    if (failureCount != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }
    std::printf("BlurMessageParser checks passed\n");
    return 0;
}
