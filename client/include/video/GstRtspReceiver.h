#pragma once

#include <gst/gst.h>
#include <gst/rtsp/gstrtspmessage.h>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <atomic>

#include "video/BlurProcessor.h"
#include "video/StreamReceiver.h"
#include "video/VideoRuntimeConfig.h"

class QThread;
class QTimer;

class GstRtspReceiver : public StreamReceiver {
    Q_OBJECT

public:
    explicit GstRtspReceiver(guintptr outputWindowHandle, GstRtspReceiverConfig config, QObject* parent = nullptr);
    ~GstRtspReceiver() override;

    void setUrl(const QString& url) override;
    void setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) override;
    void setBlurFrame(BlurFrameData frame) override;
    void setVideoPreprocessingSettings(const VideoPreprocessingSettings& settings) override;
    void setPresentationActive(bool active) override;
    void moveInternalObjectsToThread(QThread* thread) override;
    void start() override;
    void stop() override;
    void markFirstFrame();

    /// CPU에서 픽셀을 만져야 하는 구성인지. 자체 검사가 직접 부른다
    bool needsSystemMemoryChain() const;
    /// 영상 체인 bin description. 자체 검사가 직접 부른다
    QString videoChainDescription(bool systemMemoryChain) const;

private slots:
    void pollBus();

private:
    void startPipeline();
    void teardownPipeline();
    void scheduleReconnect(const QString& reason, int overrideDelayMsec = 0);
    void restartPipeline(const QString& reason);
    bool applySourceProperties(GstElement* source);
    QString decoderChain(bool downloadToSystemMemory) const;
    QString d3d11DecoderChain(bool downloadToSystemMemory) const;
    bool rebuildIfChainShapeChanged(const QString& reason);
    void applyVideoPreprocessingSettings();
    void applyBlurPassthrough();
    void applyPresentationState();
    void checkStall();
    void reportFrameStatistics();

    void markFirstPacket();

    static GstPadProbeReturn onFrameProbe(GstPad* pad, GstPadProbeInfo* info, gpointer userData);

    static GstPadProbeReturn onPacketProbe(GstPad* pad, GstPadProbeInfo* info, gpointer userData);

    static GstPadProbeReturn onSinkArrivalProbe(GstPad* pad, GstPadProbeInfo* info, gpointer userData);

    static gboolean onSelectStream(GstElement* source, guint streamNumber, GstCaps* caps, gpointer userData);

    static void onPadAdded(GstElement* source, GstPad* pad, gpointer userData);

    static gboolean onBeforeSend(GstElement* source, GstRTSPMessage* message, gpointer userData);

    static GstBusSyncReply onBusSyncMessage(GstBus* bus, GstMessage* message, gpointer userData);

private:
    guintptr outputWindowHandle_ = 0;
    GstRtspReceiverConfig config_;
    QString url_;

    GstElement* pipeline_ = nullptr;
    QTimer* busTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;

    QElapsedTimer startupTimer_;

    std::atomic<gint64> firstPacketTimeUsec_{0};
    std::atomic<gint64> lastPacketTimeUsec_{0};
    std::atomic<gint64> lastFrameTimeUsec_{0};
    std::atomic_bool gotAnyPacket_{false};
    std::atomic_bool gotAnyFrame_{false};

    /// 파이프라인 세 지점의 프레임 수. 끊김이 상류(망)/디코더/하류(GPU) 중 어디에서 나는지 가른다
    std::atomic<quint64> packetCount_{0};
    std::atomic<quint64> decodedFrameCount_{0};
    std::atomic<quint64> sinkFrameCount_{0};
    quint64 lastReportedPacketCount_ = 0;
    quint64 lastReportedDecodedCount_ = 0;
    quint64 lastReportedSinkCount_ = 0;
    QElapsedTimer statisticsTimer_;
    /// 체인에 실제로 들어간 디코더 factory 이름(설정값이 아니라 결과)
    QString activeDecoderName_;

    guintptr windowHandle_ = 0;
    int reconnectAttempts_ = 0;

    std::atomic_bool videoPadLinked_{false};
    std::atomic_bool manualStop_{true};
    std::atomic_bool teardownInProgress_{false};

    bool firstAsyncDoneReported_ = false;
    bool firstFrameReported_ = false;
    bool presentationActive_ = true;
    /// 현재 파이프라인이 프레임을 시스템 메모리로 내려받는 구성인지
    bool systemMemoryChainActive_ = true;

    BlurProcessor blurProcessor_;
};
