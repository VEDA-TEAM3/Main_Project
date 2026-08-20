// 영상 체인 선택 자체 검사.
//
// GPU 전용 경로는 "CPU가 픽셀을 만질 필요가 없을 때만" 골라야 한다. 판단을 한 곳이라도
// 놓치면(예: gamma를 빼먹으면) 사용자가 켜 둔 보정이 조용히 사라진다. 반대로 늘 CPU 경로를
// 고르면 채널당 매 프레임 GPU->CPU 다운로드와 재업로드를 다시 물게 된다.
//
// 싱크 플래그는 예전에 문자열 replace로 갈아끼웠는데, "sync=false"가 "async=false" 안쪽에도
// 걸려서 sinkSync를 켜면 async까지 같이 켜졌다. 그 회귀도 여기서 잡는다.
//
// 실행할 때 PATH는 **Qt bin을 GStreamer bin보다 앞에** 두어야 한다:
//   $env:Path = "C:\Qt\6.11.1\mingw_64\bin;C:\Program Files\gstreamer\1.0\mingw_x86_64\bin;" + $env:Path
// 반대로 두면 GStreamer가 같이 들고 다니는 libstdc++-6/libgcc_s_seh-1/libwinpthread-1이 Qt의
// MinGW 13.1 것을 가려서, gst_init 뒤 QObject 자식을 파괴하는 자리에서 heap이 깨진다
// (0xC0000374, 백트레이스는 QObjectPrivate::deleteChildren). 검사 로직과는 무관한 증상이다.

#include <gst/gst.h>

#include <QCoreApplication>
#include <QString>
#include <cassert>
#include <cstdio>

#include "video/GstRtspReceiver.h"

namespace {

GstRtspReceiverConfig baseConfig() {
    GstRtspReceiverConfig config;
    config.decoderMode = QStringLiteral("d3d11");
    config.processingWidth = 1280;
    config.processingHeight = 720;
    config.decodeQueueMaximumBuffers = 8;
    config.decodeQueueMaximumTimeMsec = 100;
    config.alignmentDelayMsec = 100;
    config.alignmentQueueMaximumTimeMsec = 450;
    config.renderQueueMaximumBuffers = 8;
    config.renderQueueMaximumTimeMsec = 200;
    return config;
}

bool needsSystemMemory(const GstRtspReceiverConfig& config, bool faceBlur, bool licensePlateBlur) {
    GstRtspReceiver receiver(0, config);
    receiver.setBlurTargetsEnabled(faceBlur, licensePlateBlur);
    return receiver.needsSystemMemoryChain();
}

QString chainFor(const GstRtspReceiverConfig& config, bool systemMemoryChain) {
    GstRtspReceiver receiver(0, config);
    return receiver.videoChainDescription(systemMemoryChain);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    const GstRtspReceiverConfig neutral = baseConfig();

    // 블러 대상이 하나라도 켜져 있으면 프레임이 시스템 메모리에 있어야 한다
    assert(needsSystemMemory(neutral, true, true));
    assert(needsSystemMemory(neutral, true, false));
    assert(needsSystemMemory(neutral, false, true));

    // 블러가 전부 꺼지고 전처리도 중립이면 GPU 경로로 갈 수 있다
    assert(!needsSystemMemory(neutral, false, false));

    // 전처리 값 셋 중 무엇이 어긋나도 CPU 경로여야 한다. GPU 쪽에 대체 요소가 없다
    GstRtspReceiverConfig brightened = baseConfig();
    brightened.preprocessing.brightness = 12;
    assert(needsSystemMemory(brightened, false, false));

    GstRtspReceiverConfig contrasted = baseConfig();
    contrasted.preprocessing.contrast = 1.15;
    assert(needsSystemMemory(contrasted, false, false));

    GstRtspReceiverConfig gammaed = baseConfig();
    gammaed.preprocessing.gamma = 1.2;
    assert(needsSystemMemory(gammaed, false, false));

    // enabled=false는 값이 남아 있어도 적용되지 않으므로 중립으로 본다
    GstRtspReceiverConfig disabled = baseConfig();
    disabled.preprocessing.enabled = false;
    disabled.preprocessing.brightness = 30;
    disabled.preprocessing.gamma = 1.4;
    assert(!needsSystemMemory(disabled, false, false));

    // GPU 경로에는 CPU 처리 요소가 하나도 없어야 한다
    const QString gpuChain = chainFor(neutral, false);
    assert(!gpuChain.contains(QStringLiteral("d3d11download")));
    assert(!gpuChain.contains(QStringLiteral("d3d11scale")));
    assert(!gpuChain.contains(QStringLiteral("videoconvert")));
    assert(!gpuChain.contains(QStringLiteral("videobalance")));
    assert(!gpuChain.contains(QStringLiteral("gammafilter")));
    assert(!gpuChain.contains(QStringLiteral("qtblur")));
    // 블러 정렬용 지연선도 같이 빠져야 한다. 남겨 두면 언더런마다 임계값만큼 더 멈춘다
    assert(!gpuChain.contains(QStringLiteral("alignmentqueue")));
    // 완충과 출력은 두 경로 모두 있어야 한다
    assert(gpuChain.contains(QStringLiteral("renderqueue")));
    assert(gpuChain.contains(QStringLiteral("d3d11videosink")));
    assert(gpuChain.contains(QStringLiteral("framewatch")));
    assert(gpuChain.contains(QStringLiteral("presentationvalve")));

    // CPU 경로에는 전부 있어야 한다
    const QString cpuChain = chainFor(neutral, true);
    assert(cpuChain.contains(QStringLiteral("d3d11download")));
    assert(cpuChain.contains(QStringLiteral("alignmentqueue")));
    assert(cpuChain.contains(QStringLiteral("min-threshold-time=100000000")));
    assert(cpuChain.contains(QStringLiteral("qtblur")));
    assert(cpuChain.contains(QStringLiteral("videobalance")));
    assert(cpuChain.contains(QStringLiteral("gammafilter")));
    assert(cpuChain.contains(QStringLiteral("renderqueue")));
    assert(cpuChain.contains(QStringLiteral("d3d11videosink")));

    // 순서가 바뀌면 파이프라인은 그대로 만들어지지만 동작이 달라진다. 축소는 다운로드보다
    // 앞이어야 전송량이 줄고, 블러는 renderqueue 뒤여야 완충이 블러 지연을 흡수한다
    assert(cpuChain.indexOf(QStringLiteral("d3d11scale")) < cpuChain.indexOf(QStringLiteral("d3d11download")));
    assert(cpuChain.indexOf(QStringLiteral("d3d11download")) < cpuChain.indexOf(QStringLiteral("alignmentqueue")));
    assert(cpuChain.indexOf(QStringLiteral("alignmentqueue")) < cpuChain.indexOf(QStringLiteral("renderqueue")));
    assert(cpuChain.indexOf(QStringLiteral("renderqueue")) < cpuChain.indexOf(QStringLiteral("videobalance")));
    assert(cpuChain.indexOf(QStringLiteral("videobalance")) < cpuChain.indexOf(QStringLiteral("gammafilter")));
    assert(cpuChain.indexOf(QStringLiteral("gammafilter")) < cpuChain.indexOf(QStringLiteral("qtblur")));
    assert(cpuChain.indexOf(QStringLiteral("qtblur")) < cpuChain.indexOf(QStringLiteral("d3d11videosink")));
    // 프레임 감시 probe는 두 경로 모두 디코더 바로 뒤에 있어야 정지 감지가 제대로 돈다
    assert(cpuChain.indexOf(QStringLiteral("framewatch")) < cpuChain.indexOf(QStringLiteral("renderqueue")));
    assert(gpuChain.indexOf(QStringLiteral("framewatch")) < gpuChain.indexOf(QStringLiteral("renderqueue")));

    // 싱크 플래그는 서로 독립이어야 한다
    GstRtspReceiverConfig synced = baseConfig();
    synced.sinkSync = true;
    synced.sinkAsync = false;
    synced.sinkQos = false;
    const QString syncedChain = chainFor(synced, true);
    assert(syncedChain.contains(QStringLiteral("sync=true")));
    assert(syncedChain.contains(QStringLiteral("async=false")));
    assert(!syncedChain.contains(QStringLiteral("async=true")));
    assert(syncedChain.contains(QStringLiteral("qos=false")));

    GstRtspReceiverConfig asynced = baseConfig();
    asynced.sinkSync = false;
    asynced.sinkAsync = true;
    const QString asyncedChain = chainFor(asynced, true);
    assert(asyncedChain.contains(QStringLiteral("async=true")));
    assert(asyncedChain.contains(QStringLiteral("sync=false")));

    std::printf("video chain selection check passed\n");
    return 0;
}
