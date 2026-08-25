#include "video/GstRtspReceiver.h"

#include <gst/base/gstbasetransform.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/videooverlay.h>

#include <QByteArray>
#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "video/BlurVideoFilter.h"
#include "video/VideoPreprocessFilter.h"

namespace {
/**
 * @brief             지정한 GStreamer element factory가 설치되어 있는지
 * 확인합니다.
 * @param factoryName  확인할 factory 이름
 * @return            factory를 찾았으면 true
 */
bool hasGstFactory(const char* factoryName) {
    GstElementFactory* factory = gst_element_factory_find(factoryName);

    if (!factory) {
        return false;
    }

    gst_object_unref(factory);
    return true;
}

/**
 * @brief       caps가 H.264 RTP video stream인지 판정합니다.
 * @param caps  검사할 GstCaps
 * @return      H.264 video caps이면 true
 */
bool isH264VideoCaps(GstCaps* caps) {
    if (!caps || gst_caps_is_empty(caps)) {
        return false;
    }

    const GstStructure* structure = gst_caps_get_structure(caps, 0);

    if (!structure) {
        return false;
    }

    const char* media = gst_structure_get_string(structure, "media");
    const char* encodingName = gst_structure_get_string(structure, "encoding-name");

    return media && encodingName && std::strcmp(media, "video") == 0 && std::strcmp(encodingName, "H264") == 0;
}

/**
 * @brief      동적 pad의 caps를 조회해 H.264 video pad인지 확인합니다.
 * @param pad  검사할 GstPad
 * @return     H.264 video pad이면 true
 */
bool isH264VideoPad(GstPad* pad) {
    if (!pad) {
        return false;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);

    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }

    const bool result = isH264VideoCaps(caps);

    if (caps) {
        gst_caps_unref(caps);
    }

    return result;
}

/**
 * @brief               element가 지원하는 경우에만 boolean property를
 * 설정합니다.
 * @param element       대상 GstElement
 * @param propertyName  설정할 property 이름
 * @param value         설정할 값
 */
void setOptionalBooleanProperty(GstElement* element, const char* propertyName, gboolean value) {
    if (!element || !propertyName) {
        return;
    }

    GParamSpec* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName);

    if (!spec) {
        return;
    }

    g_object_set(element, propertyName, value, nullptr);
}

/**
 * @brief      RTSP URL에 배포 전 비밀번호 placeholder가 남아있는지 확인합니다.
 * @param url  검사할 RTSP URL
 * @return     placeholder가 남아있으면 true
 */
bool hasCredentialPlaceholder(const QString& url) {
    return url.contains(QStringLiteral(":PASSWORD@"), Qt::CaseInsensitive);
}

/**
 * @brief            GStreamer 오류와 debug 문자열을 사용자 로그용 문구로
 * 정리합니다.
 * @param error      GStreamer 오류 객체
 * @param debugInfo  GStreamer debug 문자열
 * @return           정규화된 오류 메시지
 */
QString normalizedGstErrorText(GError* error, const gchar* debugInfo) {
    const QString message = error ? QString::fromUtf8(error->message) : QStringLiteral("Unknown GStreamer error");
    const QString debugText = debugInfo ? QString::fromUtf8(debugInfo) : QString();

    if (debugText.contains(QStringLiteral("Account Blocked"), Qt::CaseInsensitive)) {
        return QStringLiteral(
            "RTSP account blocked (490): check password or wait "
            "for NVR account unlock");
    }

    if (debugText.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive) ||
        debugText.contains(QStringLiteral("(401)"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive)) {
        return QStringLiteral("RTSP authentication failed: check user name/password");
    }

    if (message.compare(QStringLiteral("Unhandled error"), Qt::CaseInsensitive) == 0 && !debugText.isEmpty()) {
        return QStringLiteral("RTSP error: %1").arg(debugText.section(QLatin1Char('\n'), -1).trimmed());
    }

    return message;
}

/**
 * @brief         rtspsrc가 실제로 어느 하위 transport로 붙었는지 판별합니다.
 * @param source  PLAYING 상태의 rtspsrc element
 * @return        내부에 udpsrc가 있으면 "UDP", 없으면 "TCP(interleaved)"
 *
 * @details rtspsrc의 protocols 기본값은 UDP를 먼저 시도하고 실패하면 TCP로 넘어가므로, 로그가
 *          없으면 끊김이 망 손실인지 로컬 부하인지 가를 근거가 없다. UDP transport일 때만
 *          rtspsrc가 내부에 udpsrc를 만들기 때문에 그 존재로 판별한다.
 */
QString negotiatedTransportName(GstElement* source) {
    if (!source || !GST_IS_BIN(source)) {
        return QStringLiteral("unknown");
    }

    GstIterator* iterator = gst_bin_iterate_recurse(GST_BIN(source));

    if (!iterator) {
        return QStringLiteral("unknown");
    }

    bool hasUdpSource = false;
    bool done = false;
    GValue item = G_VALUE_INIT;

    while (!done) {
        switch (gst_iterator_next(iterator, &item)) {
            case GST_ITERATOR_OK: {
                auto* element = static_cast<GstElement*>(g_value_get_object(&item));
                GstElementFactory* factory = element ? gst_element_get_factory(element) : nullptr;
                const gchar* factoryName = factory ? GST_OBJECT_NAME(factory) : nullptr;

                if (factoryName && std::strcmp(factoryName, "udpsrc") == 0) {
                    hasUdpSource = true;
                    done = true;
                }

                g_value_reset(&item);
                break;
            }

            case GST_ITERATOR_RESYNC:
                hasUdpSource = false;
                gst_iterator_resync(iterator);
                break;

            default:
                done = true;
                break;
        }
    }

    g_value_unset(&item);
    gst_iterator_free(iterator);

    return hasUdpSource ? QStringLiteral("UDP") : QStringLiteral("TCP(interleaved)");
}

/**
 * @brief            오류 문구가 인증 실패 계열인지 확인합니다.
 * @param errorText  정규화된 오류 메시지
 * @return           인증 실패이면 true
 */
bool isAuthenticationFailure(const QString& errorText) {
    return errorText.contains(QStringLiteral("account blocked"), Qt::CaseInsensitive) ||
           errorText.contains(QStringLiteral("authentication failed"), Qt::CaseInsensitive);
}

}  // namespace

/**
 * @brief                    RTSP 수신기를 생성하고 재연결/버스 타이머를
 * 준비합니다.
 * @param outputWindowHandle  영상을 출력할 네이티브 윈도우 핸들
 * @param parent              Qt 객체 소유권 부모
 */
GstRtspReceiver::GstRtspReceiver(guintptr outputWindowHandle, GstRtspReceiverConfig config, QObject* parent)
    : StreamReceiver(parent),
      outputWindowHandle_(outputWindowHandle),
      config_(std::move(config)),
      blurProcessor_(config_.blur) {
    busTimer_ = new QTimer(this);
    reconnectTimer_ = new QTimer(this);
    busTimer_->setTimerType(Qt::PreciseTimer);
    reconnectTimer_->setSingleShot(true);

    connect(busTimer_, &QTimer::timeout, this, &GstRtspReceiver::pollBus);

    connect(reconnectTimer_, &QTimer::timeout, this, &GstRtspReceiver::startPipeline);
}

/**
 * @brief   수신기를 정지하고 내부 pipeline을 해제합니다.
 */
GstRtspReceiver::~GstRtspReceiver() { stop(); }

/**
 * @brief      수신할 RTSP URL을 설정합니다.
 * @param url  RTSP 주소
 */
void GstRtspReceiver::setUrl(const QString& url) { url_ = url.trimmed(); }

void GstRtspReceiver::setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) {
    blurProcessor_.setTargetsEnabled(faceEnabled, licensePlateEnabled);

    if (rebuildIfChainShapeChanged(QStringLiteral("blur targets changed"))) {
        return;
    }

    applyBlurPassthrough();
}

void GstRtspReceiver::setBlurFrame(BlurFrameData frame) { blurProcessor_.submitFrame(std::move(frame)); }

/**
 * @brief          현재 채널의 영상 전처리 값을 저장하고 실행 중인 필터에
 * 반영합니다.
 * @param settings 적용할 영상 전처리 설정
 */
void GstRtspReceiver::setVideoPreprocessingSettings(const VideoPreprocessingSettings& settings) {
    config_.preprocessing = settings;

    if (rebuildIfChainShapeChanged(QStringLiteral("preprocessing changed"))) {
        return;
    }

    applyVideoPreprocessingSettings();
}

/**
 * @brief        CPU 처리가 필요한지 여부가 뒤집혔으면 파이프라인을 다시 세웁니다.
 * @param reason 로그와 상태 표시에 남길 사유
 * @return       재시작을 시작했으면 true
 *
 * @details 다운로드 요소를 넣고 빼는 것은 실행 중에 링크를 바꿔서는 할 수 없으므로 재연결로
 *          처리한다. 블러 토글과 전처리 값은 설정 팝업에서만 바뀌는 드문 조작이라 그 자리에서
 *          한 번 끊기는 편이, 필요도 없는 프레임 왕복을 세션 내내 무는 것보다 싸다.
 */
bool GstRtspReceiver::rebuildIfChainShapeChanged(const QString& reason) {
    if (!pipeline_ || needsSystemMemoryChain() == systemMemoryChainActive_) {
        return false;
    }

    restartPipeline(reason);
    return true;
}

/**
 * @brief        디코더 이후의 영상 처리와 화면 출력을 전환합니다.
 * @param active true면 화면 출력, false면 RTSP와 디코더만 워밍 상태로 유지
 */
void GstRtspReceiver::setPresentationActive(bool active) {
    if (presentationActive_ == active) {
        return;
    }

    presentationActive_ = active;
    if (active) {
        const gint64 nowUsec = g_get_monotonic_time();
        lastFrameTimeUsec_.store(nowUsec, std::memory_order_relaxed);
        if (!gotAnyFrame_.load(std::memory_order_relaxed)) {
            firstPacketTimeUsec_.store(nowUsec, std::memory_order_release);
        }
    }
    applyPresentationState();
}

/**
 * @brief        수신기 본체와 자식 타이머를 지정한 worker 스레드로 이동합니다.
 * @param thread  이동 대상 QThread
 */
void GstRtspReceiver::moveInternalObjectsToThread(QThread* thread) {
    if (!thread) {
        return;
    }

    if (QThread::currentThread() != this->thread()) {
        qWarning() << "[GstRtspReceiver] moveToThread must be called from the "
                      "receiver's current thread";
        return;
    }

    if (!moveToThread(thread)) {
        qWarning() << "[GstRtspReceiver] Failed to move receiver to target thread";
    }
}

/**
 * @brief   수동 정지 상태를 해제하고 pipeline 시작을 요청합니다.
 */
void GstRtspReceiver::start() {
    manualStop_.store(false, std::memory_order_release);
    reconnectAttempts_ = 0;

    reconnectTimer_->stop();

    startPipeline();
}

/**
 * @brief   rtspsrc와 영상 처리 chain을 구성하고 PLAYING 상태로 전환합니다.
 */
void GstRtspReceiver::startPipeline() {
    if (manualStop_.load(std::memory_order_acquire)) {
        return;
    }

    if (outputWindowHandle_ == 0) {
        emit errorOccurred("Output window handle is invalid");
        return;
    }

    if (url_.isEmpty()) {
        emit errorOccurred("RTSP URL is empty");
        return;
    }

    if (hasCredentialPlaceholder(url_)) {
        emit loadingChanged(false);
        emit errorOccurred(QStringLiteral("RTSP password placeholder is still set in app_config.json"));
        emit statusChanged(QStringLiteral("RTSP configuration error"));
        return;
    }

    teardownPipeline();

    firstAsyncDoneReported_ = false;
    firstFrameReported_ = false;
    videoPadLinked_.store(false, std::memory_order_release);
    teardownInProgress_.store(false, std::memory_order_release);

    gotAnyPacket_.store(false, std::memory_order_relaxed);
    gotAnyFrame_.store(false, std::memory_order_relaxed);

    const gint64 startTimeUsec = g_get_monotonic_time();

    firstPacketTimeUsec_.store(0, std::memory_order_relaxed);
    lastPacketTimeUsec_.store(startTimeUsec, std::memory_order_relaxed);
    lastFrameTimeUsec_.store(startTimeUsec, std::memory_order_relaxed);

    startupTimer_.restart();
    emit loadingChanged(true);

    windowHandle_ = outputWindowHandle_;

    if (!windowHandle_) {
        emit errorOccurred("Invalid video window handle");
        scheduleReconnect("invalid video window handle");
        return;
    }

    if (!BlurVideoFilter::ensureRegistered()) {
        emit errorOccurred("Failed to register blur video filter");
        scheduleReconnect("blur filter registration failed");
        return;
    }

    if (!VideoPreprocessFilter::ensureRegistered()) {
        emit errorOccurred("Failed to register video preprocess filter");
        scheduleReconnect("preprocess filter registration failed");
        return;
    }

    systemMemoryChainActive_ = needsSystemMemoryChain();
    const QString videoChainDesc = videoChainDescription(systemMemoryChainActive_);

    qDebug().noquote() << "[GstRtspReceiver] Manual RTSP pipeline:" << videoChainDesc;
    const QString processingSize =
        config_.processingWidth > 0 && config_.processingHeight > 0
            ? QStringLiteral("%1x%2").arg(config_.processingWidth).arg(config_.processingHeight)
            : QStringLiteral("source");
    const QString pathName =
        systemMemoryChainActive_ ? QStringLiteral("cpu(download)") : QStringLiteral("gpu(no-download)");
    const qint64 playoutDelayMsec = systemMemoryChainActive_ ? config_.alignmentDelayMsec : 0;
    qInfo().noquote() << QStringLiteral(
                             "[GstRtspReceiver] video path=%1 sinkSync=%2 tsOffset=%3ms renderQueue=%4ms "
                             "processing=%5")
                             .arg(pathName)
                             .arg(config_.sinkSync)
                             .arg(playoutDelayMsec)
                             .arg(config_.renderQueueMaximumTimeMsec + playoutDelayMsec)
                             .arg(systemMemoryChainActive_ ? processingSize : QStringLiteral("source"));

    GError* error = nullptr;
    GstElement* source = gst_element_factory_make("rtspsrc", "src");
    GstElement* videoChain = gst_parse_bin_from_description(videoChainDesc.toUtf8().constData(), TRUE, &error);

    if (!source || !videoChain) {
        const QString msg = error ? QString::fromUtf8(error->message) : "Failed to create RTSP video chain";

        emit errorOccurred(msg);

        if (error) {
            g_error_free(error);
        }

        if (source) {
            gst_object_unref(source);
        }

        if (videoChain) {
            gst_object_unref(videoChain);
        }

        scheduleReconnect(msg);
        return;
    }

    gst_element_set_name(videoChain, "videochain");

    if (error) {
        qDebug().noquote() << "[GStreamer Parse Warning]" << QString::fromUtf8(error->message);
        g_error_free(error);
    }

    pipeline_ = gst_pipeline_new(nullptr);

    if (!pipeline_) {
        emit errorOccurred("Failed to create pipeline");
        gst_object_unref(source);
        gst_object_unref(videoChain);
        scheduleReconnect("pipeline creation failed");
        return;
    }

    if (!applySourceProperties(source)) {
        gst_object_unref(source);
        gst_object_unref(videoChain);
        teardownPipeline();
        scheduleReconnect("invalid RTSP URL");
        return;
    }

    g_object_set(source, "latency", config_.latencyMsec, "drop-on-latency", config_.dropOnLatency ? TRUE : FALSE,
                 "tcp-timeout", static_cast<guint64>(config_.tcpTimeoutUsec), "timeout",
                 static_cast<guint64>(config_.udpTimeoutUsec), "probation", config_.probationPackets, "udp-buffer-size",
                 static_cast<guint>(config_.udpBufferSizeBytes), nullptr);
    setOptionalBooleanProperty(source, "do-rtsp-keep-alive", config_.rtspKeepAlive ? TRUE : FALSE);
    setOptionalBooleanProperty(source, "udp-reconnect", config_.udpReconnect ? TRUE : FALSE);
    setOptionalBooleanProperty(source, "add-reference-timestamp-meta",
                               config_.addReferenceTimestampMeta ? TRUE : FALSE);

    gst_bin_add_many(GST_BIN(pipeline_), source, videoChain, nullptr);

    g_signal_connect(source, "select-stream", G_CALLBACK(&GstRtspReceiver::onSelectStream), this);
    g_signal_connect(source, "pad-added", G_CALLBACK(&GstRtspReceiver::onPadAdded), this);
    g_signal_connect(source, "before-send", G_CALLBACK(&GstRtspReceiver::onBeforeSend), this);
    qDebug().noquote() << "[GstRtspReceiver] rtspsrc latency:" << config_.latencyMsec
                       << "drop-on-latency:" << config_.dropOnLatency << "protocols:defaults";

    if (GstBus* bus = gst_element_get_bus(pipeline_)) {
        gst_bus_set_sync_handler(bus, &GstRtspReceiver::onBusSyncMessage, this, nullptr);
        gst_object_unref(bus);
    }

    GstElement* videoChainForProbe = gst_bin_get_by_name(GST_BIN(pipeline_), "videochain");
    GstElement* depay = nullptr;
    GstElement* blur = nullptr;
    GstElement* framewatch = nullptr;

    if (videoChainForProbe && GST_IS_BIN(videoChainForProbe)) {
        depay = gst_bin_get_by_name(GST_BIN(videoChainForProbe), "depay");
        blur = gst_bin_get_by_name(GST_BIN(videoChainForProbe), "blur");
        framewatch = gst_bin_get_by_name(GST_BIN(videoChainForProbe), "framewatch");
    }

    applyVideoPreprocessingSettings();
    applyBlurPassthrough();
    applyPresentationState();

    if (blur) {
        BlurVideoFilter::setProcessor(blur, &blurProcessor_);
        gst_object_unref(blur);
    } else if (systemMemoryChainActive_) {
        // GPU 경로에는 qtblur가 애초에 없다. 없어야 정상인 경우까지 경고하지 않는다
        qWarning() << "[GstRtspReceiver] Failed to find blur filter";
    }

    if (depay) {
        if (GstPad* depaySinkPad = gst_element_get_static_pad(depay, "sink")) {
            gst_pad_add_probe(depaySinkPad, GST_PAD_PROBE_TYPE_BUFFER, &GstRtspReceiver::onPacketProbe, this, nullptr);
            gst_object_unref(depaySinkPad);
        }

        gst_object_unref(depay);
    } else {
        qWarning() << "[GstRtspReceiver] Failed to find depay";
    }

    if (framewatch) {
        if (GstPad* framewatchSrcPad = gst_element_get_static_pad(framewatch, "src")) {
            gst_pad_add_probe(framewatchSrcPad, GST_PAD_PROBE_TYPE_BUFFER, &GstRtspReceiver::onFrameProbe, this,
                              nullptr);
            gst_object_unref(framewatchSrcPad);
        }

        gst_object_unref(framewatch);
    } else {
        qWarning() << "[GstRtspReceiver] Failed to find framewatch";
    }

    if (videoChainForProbe) {
        gst_object_unref(videoChainForProbe);
    }

    GstElement* sink = nullptr;
    GstElement* videoChainForSink = gst_bin_get_by_name(GST_BIN(pipeline_), "videochain");

    if (videoChainForSink && GST_IS_BIN(videoChainForSink)) {
        sink = gst_bin_get_by_name(GST_BIN(videoChainForSink), "videosink");
    }

    if (videoChainForSink) {
        gst_object_unref(videoChainForSink);
    }

    if (!sink) {
        emit errorOccurred("Failed to find videosink");
        teardownPipeline();
        scheduleReconnect("videosink not found");
        return;
    }

    if (!GST_IS_VIDEO_OVERLAY(sink)) {
        emit errorOccurred("videosink does not support GstVideoOverlay");
        gst_object_unref(sink);
        teardownPipeline();
        scheduleReconnect("videosink overlay unsupported");
        return;
    }

    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), windowHandle_);
    gst_object_unref(sink);

    const GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit errorOccurred("Failed to set pipeline to PLAYING");
        teardownPipeline();
        scheduleReconnect("state change failure");
        return;
    }

    busTimer_->start(config_.busPollIntervalMsec);

    emit statusChanged("Connecting");
}

/**
 * @brief   재연결 예약을 취소하고 현재 pipeline을 종료합니다.
 */
void GstRtspReceiver::stop() {
    manualStop_.store(true, std::memory_order_release);

    reconnectTimer_->stop();

    teardownPipeline();
    emit loadingChanged(false);
}

/**
 * @brief   GStreamer pipeline을 NULL 상태로 내린 뒤 bus handler와 참조를
 * 정리합니다.
 */
void GstRtspReceiver::teardownPipeline() {
    busTimer_->stop();

    if (pipeline_) {
        GstElement* pipeline = pipeline_;
        pipeline_ = nullptr;

        if (GstBus* bus = gst_element_get_bus(pipeline)) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }

        teardownInProgress_.store(true, std::memory_order_release);
        gst_element_set_state(pipeline, GST_STATE_NULL);

        const GstStateChangeReturn ret = gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
        teardownInProgress_.store(false, std::memory_order_release);

        if (ret == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "[GstRtspReceiver] Failed to set pipeline to NULL";
        } else if (ret == GST_STATE_CHANGE_ASYNC) {
            qWarning() << "[GstRtspReceiver] Timed out while waiting for pipeline "
                          "NULL state";
        }

        gst_object_unref(pipeline);
    }

    windowHandle_ = 0;

    blurProcessor_.clear();
}

/**
 * @brief                  지수 backoff 규칙에 따라 다음 RTSP 재연결을
 * 예약합니다.
 * @param reason            재연결 사유
 * @param overrideDelayMsec  0보다 크면 기본 backoff 대신 사용할 지연 시간
 */
void GstRtspReceiver::scheduleReconnect(const QString& reason, int overrideDelayMsec) {
    if (manualStop_.load(std::memory_order_acquire) || reconnectTimer_->isActive()) {
        return;
    }

    const int backoffStep = std::min(reconnectAttempts_, 4);
    const int baseDelayMsec = std::min(config_.maximumReconnectDelayMsec, 1000 << backoffStep);
    const int channelSpreadMsec = static_cast<int>(qHash(url_) % config_.reconnectSpreadMsec);
    const int delayMsec = overrideDelayMsec > 0 ? overrideDelayMsec : baseDelayMsec + channelSpreadMsec;
    ++reconnectAttempts_;

    emit loadingChanged(true);

    emit statusChanged(
        QString("Reconnect in %1 ms (attempt %2): %3").arg(delayMsec).arg(reconnectAttempts_).arg(reason));

    reconnectTimer_->start(delayMsec);
}

/**
 * @brief         현재 pipeline을 정리하고 재연결을 예약합니다.
 * @param reason  재시작 사유
 */
void GstRtspReceiver::restartPipeline(const QString& reason) {
    qDebug().noquote() << "[GstRtspReceiver] Restart:" << reason;
    emit statusChanged(reason);

    teardownPipeline();
    scheduleReconnect(reason);
}

/**
 * @brief         URL의 사용자 정보를 rtspsrc property로 분리해 적용합니다.
 * @param source  설정할 rtspsrc element
 * @return        설정에 성공하면 true
 */
bool GstRtspReceiver::applySourceProperties(GstElement* source) {
    if (!source) {
        return false;
    }

    QUrl rtspUrl(url_);
    QString location = url_;
    QString userName;
    QString password;

    if (rtspUrl.isValid() && !rtspUrl.scheme().isEmpty()) {
        userName = rtspUrl.userName(QUrl::FullyDecoded);
        password = rtspUrl.password(QUrl::FullyDecoded);
        location = rtspUrl.toString(QUrl::RemoveUserInfo);
    }

    if (location.trimmed().isEmpty()) {
        emit errorOccurred("RTSP location is empty");
        return false;
    }

    const QByteArray locationBytes = location.toUtf8();
    g_object_set(source, "location", locationBytes.constData(), nullptr);

    if (!userName.isEmpty()) {
        const QByteArray userBytes = userName.toUtf8();
        g_object_set(source, "user-id", userBytes.constData(), nullptr);
    }

    if (!password.isEmpty()) {
        const QByteArray passwordBytes = password.toUtf8();
        g_object_set(source, "user-pw", passwordBytes.constData(), nullptr);
    }

    return true;
}

/**
 * @brief   첫 RTP/H.264 패킷 수신을 UI 상태와 로그로 알립니다.
 */
void GstRtspReceiver::markFirstPacket() {
    if (manualStop_.load(std::memory_order_acquire) || !pipeline_) {
        return;
    }

    const qint64 elapsed = startupTimer_.isValid() ? startupTimer_.elapsed() : 0;
    QString transport = QStringLiteral("unknown");

    if (GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline_), "src")) {
        transport = negotiatedTransportName(source);
        gst_object_unref(source);
    }

    qInfo().noquote()
        << QStringLiteral("[GstRtspReceiver] RTSP transport=%1 firstPacket=%2 ms").arg(transport).arg(elapsed);
    emit statusChanged(QString("First RTP/H264 packet in %1 ms (%2)").arg(elapsed).arg(transport));
}

/**
 * @brief   첫 디코딩 프레임 수신을 기록하고 최소 로딩 연출 이후 오버레이를
 * 숨깁니다.
 */
void GstRtspReceiver::markFirstFrame() {
    if (firstFrameReported_ || manualStop_.load(std::memory_order_acquire) || !pipeline_) {
        return;
    }

    firstFrameReported_ = true;
    reconnectAttempts_ = 0;

    const qint64 elapsed = startupTimer_.isValid() ? startupTimer_.elapsed() : config_.minimumLoadingMsec;
    const int remainingMsec = static_cast<int>(std::max<qint64>(0, config_.minimumLoadingMsec - elapsed));

    emit statusChanged(QString("First frame in %1 ms").arg(elapsed));
    emit firstFrameReceived();

    QTimer::singleShot(remainingMsec, this, [this]() {
        if (!manualStop_.load(std::memory_order_acquire) && firstFrameReported_) {
            emit loadingChanged(false);
        }
    });
}

/**
 * @brief   초기 패킷/프레임 수신 지연과 실행 중 frame stall을 감시합니다.
 */
void GstRtspReceiver::checkStall() {
    if (manualStop_.load(std::memory_order_acquire) || !pipeline_) {
        return;
    }

    const gint64 nowUsec = g_get_monotonic_time();
    const qint64 startupElapsedMsec = startupTimer_.isValid() ? startupTimer_.elapsed() : 0;

    if (!gotAnyPacket_.load(std::memory_order_relaxed)) {
        if (startupElapsedMsec > config_.initialPacketTimeoutMsec) {
            const QString stage = videoPadLinked_.load(std::memory_order_acquire)
                                      ? QStringLiteral("H264 pad linked but no depay packet arrived")
                                      : QStringLiteral("no H264 RTP pad/packet arrived");
            const QString reason =
                QStringLiteral("initial stream timeout after %1 ms: %2").arg(startupElapsedMsec).arg(stage);

            restartPipeline(reason);
        }

        return;
    }

    if (!presentationActive_) {
        const gint64 packetAgeMsec = (nowUsec - lastPacketTimeUsec_.load(std::memory_order_relaxed)) / 1000;
        if (packetAgeMsec > config_.stallTimeoutMsec) {
            restartPipeline(QStringLiteral("warm stream stalled; packetAge=%1 ms").arg(packetAgeMsec));
        }
        return;
    }

    if (!gotAnyFrame_.load(std::memory_order_relaxed)) {
        const gint64 firstPacketTime = firstPacketTimeUsec_.load(std::memory_order_acquire);

        if (firstPacketTime <= 0) {
            return;
        }

        const gint64 elapsedSinceFirstPacketMsec = (nowUsec - firstPacketTime) / 1000;

        if (elapsedSinceFirstPacketMsec > config_.initialFrameTimeoutMsec) {
            const gint64 packetAgeMsec = (nowUsec - lastPacketTimeUsec_.load(std::memory_order_relaxed)) / 1000;
            const QString reason = QStringLiteral(
                                       "no decoded frame for %1 ms after "
                                       "first RTP packet; packetAge=%2 ms")
                                       .arg(elapsedSinceFirstPacketMsec)
                                       .arg(packetAgeMsec);

            restartPipeline(reason);
        }

        return;
    }

    const gint64 lastFrameTime = lastFrameTimeUsec_.load(std::memory_order_relaxed);

    if (lastFrameTime <= 0) {
        return;
    }

    const gint64 elapsedMsec = (nowUsec - lastFrameTime) / 1000;

    if (elapsedMsec <= config_.stallTimeoutMsec) {
        return;
    }

    const gint64 packetAgeMsec = (nowUsec - lastPacketTimeUsec_.load(std::memory_order_relaxed)) / 1000;
    const QString reason =
        QStringLiteral("stream stalled for %1 ms; packetAge=%2 ms").arg(elapsedMsec).arg(packetAgeMsec);

    restartPipeline(reason);
}

/**
 * @brief            디코딩된 프레임 buffer 수신 시각을 기록합니다.
 * @param userData   GstRtspReceiver 포인터
 * @return           pad probe 처리 결과
 */
GstPadProbeReturn GstRtspReceiver::onFrameProbe(GstPad*, GstPadProbeInfo*, gpointer userData) {
    auto* receiver = static_cast<GstRtspReceiver*>(userData);

    if (!receiver) {
        return GST_PAD_PROBE_OK;
    }

    receiver->lastFrameTimeUsec_.store(g_get_monotonic_time(), std::memory_order_relaxed);

    bool expected = false;
    if (receiver->gotAnyFrame_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        QMetaObject::invokeMethod(receiver, [receiver]() { receiver->markFirstFrame(); }, Qt::QueuedConnection);
    }

    return GST_PAD_PROBE_OK;
}

/**
 * @brief            depayloader 입력 패킷 수신 시각을 기록합니다.
 * @param userData   GstRtspReceiver 포인터
 * @return           pad probe 처리 결과
 */
GstPadProbeReturn GstRtspReceiver::onPacketProbe(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* receiver = static_cast<GstRtspReceiver*>(userData);

    if (!receiver) {
        return GST_PAD_PROBE_OK;
    }

    if (info) {
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        receiver->blurProcessor_.observeVideoBuffer(buffer);
    }

    const gint64 packetTimeUsec = g_get_monotonic_time();
    receiver->lastPacketTimeUsec_.store(packetTimeUsec, std::memory_order_relaxed);

    gint64 expectedFirstPacketTime = 0;
    receiver->firstPacketTimeUsec_.compare_exchange_strong(expectedFirstPacketTime, packetTimeUsec,
                                                           std::memory_order_release, std::memory_order_relaxed);

    bool expected = false;
    if (receiver->gotAnyPacket_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
        QMetaObject::invokeMethod(receiver, [receiver]() { receiver->markFirstPacket(); }, Qt::QueuedConnection);
    }

    return GST_PAD_PROBE_OK;
}

/**
 * @brief                rtspsrc의 여러 stream 중 H.264 video stream만
 * 선택합니다.
 * @param streamNumber   RTSP stream 번호
 * @param caps           stream caps 정보
 * @return               선택할 stream이면 TRUE
 */
gboolean GstRtspReceiver::onSelectStream(GstElement*, guint streamNumber, GstCaps* caps, gpointer) {
    const bool selected = isH264VideoCaps(caps);
    gchar* capsText = caps ? gst_caps_to_string(caps) : g_strdup("(null)");

    qDebug().noquote() << QString("[GstRtspReceiver] select-stream #%1").arg(streamNumber)
                       << (selected ? "H264 video selected" : "ignored") << QString::fromUtf8(capsText);

    g_free(capsText);

    return selected ? TRUE : FALSE;
}

/**
 * @brief           rtspsrc 동적 pad 중 H.264 video pad를 video chain에
 * 연결합니다.
 * @param pad       새로 추가된 rtspsrc pad
 * @param userData  GstRtspReceiver 포인터
 */
void GstRtspReceiver::onPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* receiver = static_cast<GstRtspReceiver*>(userData);

    if (!receiver || receiver->manualStop_.load(std::memory_order_acquire) || !receiver->pipeline_) {
        return;
    }

    if (receiver->videoPadLinked_.load(std::memory_order_acquire)) {
        return;
    }

    if (!isH264VideoPad(pad)) {
        qDebug().noquote() << "[GstRtspReceiver] Ignored non-H264 RTSP pad";
        return;
    }

    GstElement* videoChain = gst_bin_get_by_name(GST_BIN(receiver->pipeline_), "videochain");

    if (!videoChain) {
        QMetaObject::invokeMethod(
            receiver, [receiver]() { receiver->errorOccurred("videochain not found"); }, Qt::QueuedConnection);
        return;
    }

    GstPad* chainSinkPad = gst_element_get_static_pad(videoChain, "sink");

    if (!chainSinkPad) {
        gst_object_unref(videoChain);
        QMetaObject::invokeMethod(
            receiver, [receiver]() { receiver->errorOccurred("videochain sink pad not found"); }, Qt::QueuedConnection);
        return;
    }

    if (gst_pad_is_linked(chainSinkPad)) {
        receiver->videoPadLinked_.store(true, std::memory_order_release);
        gst_object_unref(chainSinkPad);
        gst_object_unref(videoChain);
        return;
    }

    const GstPadLinkReturn linkResult = gst_pad_link(pad, chainSinkPad);

    if (GST_PAD_LINK_SUCCESSFUL(linkResult)) {
        receiver->videoPadLinked_.store(true, std::memory_order_release);
        QMetaObject::invokeMethod(
            receiver, [receiver]() { receiver->statusChanged(QStringLiteral("H264 video pad linked")); },
            Qt::QueuedConnection);
    } else {
        const QString errorText = QString("Failed to link H264 video pad: %1").arg(gst_pad_link_get_name(linkResult));
        QMetaObject::invokeMethod(
            receiver, [receiver, errorText]() { receiver->errorOccurred(errorText); }, Qt::QueuedConnection);
    }

    gst_object_unref(chainSinkPad);
    gst_object_unref(videoChain);
}

/**
 * @brief           pipeline 종료 중 PAUSE 요청을 막아 rtspsrc가 TEARDOWN으로
 * 닫히도록 유도합니다.
 * @param message   전송 직전의 RTSP message
 * @param userData  GstRtspReceiver 포인터
 * @return          message 전송을 유지하려면 TRUE
 */
gboolean GstRtspReceiver::onBeforeSend(GstElement*, GstRTSPMessage* message, gpointer userData) {
    auto* receiver = static_cast<GstRtspReceiver*>(userData);

    if (!receiver || !receiver->teardownInProgress_.load(std::memory_order_acquire) || !message) {
        return TRUE;
    }

    if (gst_rtsp_message_get_type(message) != GST_RTSP_MESSAGE_REQUEST) {
        return TRUE;
    }

    GstRTSPMethod method = GST_RTSP_INVALID;
    const gchar* uri = nullptr;
    GstRTSPVersion version;

    if (gst_rtsp_message_parse_request(message, &method, &uri, &version) != GST_RTSP_OK) {
        return TRUE;
    }

    if (method != GST_RTSP_PAUSE) {
        return TRUE;
    }

    qDebug().noquote() << "[GstRtspReceiver] Drop RTSP PAUSE while tearing down; "
                          "let rtspsrc close with TEARDOWN";
    return FALSE;
}

/**
 * @brief           video sink가 window handle을 요청하는 sync message를 즉시
 * 처리합니다.
 * @param message   GStreamer bus message
 * @param userData  GstRtspReceiver 포인터
 * @return          bus sync 처리 결과
 */
GstBusSyncReply GstRtspReceiver::onBusSyncMessage(GstBus*, GstMessage* message, gpointer userData) {
    auto* receiver = static_cast<GstRtspReceiver*>(userData);

    if (!receiver || !message) {
        return GST_BUS_PASS;
    }

    if (gst_is_video_overlay_prepare_window_handle_message(message) && receiver->windowHandle_ != 0 &&
        GST_IS_VIDEO_OVERLAY(GST_MESSAGE_SRC(message))) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)), receiver->windowHandle_);
    }

    return GST_BUS_PASS;
}

/**
 * @brief                          사용 가능한 H.264 decoder chain 문자열을 반환합니다.
 * @param downloadToSystemMemory   true면 디코딩 결과를 시스템 메모리로 내려받는 체인
 * @return                         GStreamer bin description 일부로 사용할 decoder chain
 */
QString GstRtspReceiver::decoderChain(bool downloadToSystemMemory) const {
    const QByteArray decoderMode = config_.decoderMode.toLatin1();
    const bool d3d11Available = hasGstFactory("d3d11h264dec") && hasGstFactory("d3d11download");

    if (d3d11Available && (decoderMode == "d3d11" || !hasGstFactory("avdec_h264"))) {
        return d3d11DecoderChain(downloadToSystemMemory);
    }

    if (decoderMode != "d3d11" && hasGstFactory("avdec_h264")) {
        return "avdec_h264 max-threads=2 ! video/x-raw,format=I420";
    }

    if (d3d11Available) {
        return d3d11DecoderChain(downloadToSystemMemory);
    }

    return "avdec_h264 max-threads=2 ! video/x-raw,format=I420";
}

/**
 * @brief                          d3d11 하드웨어 디코더 체인을 반환합니다.
 * @param downloadToSystemMemory   true면 GPU 축소 + 시스템 메모리 다운로드까지 붙인다
 * @return                         디코더 체인
 *
 * @details 축소는 반드시 d3d11download **앞**에 둔다. 뒤에 두면 이미 원본 해상도를 CPU로
 *          내려받은 뒤라 전송량이 그대로고, 축소 비용만 CPU에 더 얹힌다.
 *          너비와 높이를 모두 지정해도 d3d11scale이 pixel-aspect-ratio로 화면비를 보정하므로
 *          4:3 카메라도 sink의 force-aspect-ratio=true와 함께 올바르게 표시된다.
 *
 *          다운로드하지 않는 GPU 경로에서는 축소도 붙이지 않는다. 축소의 목적이 전송량을
 *          줄이는 것인데 전송 자체가 없고, d3d11videosink가 어차피 창 크기로 한 번 더 늘리므로
 *          중간에 720p를 거치면 GPU 패스만 하나 늘고 화질은 떨어진다.
 */
QString GstRtspReceiver::d3d11DecoderChain(bool downloadToSystemMemory) const {
    QString chain = QStringLiteral(
        "d3d11h264dec discard-corrupted-frames=true "
        "automatic-request-sync-points=true");

    if (!downloadToSystemMemory) {
        return chain;
    }

    if (config_.processingWidth > 0 && config_.processingHeight > 0 && hasGstFactory("d3d11scale")) {
        chain += QStringLiteral(" ! d3d11scale ! video/x-raw(memory:D3D11Memory),width=%1,height=%2")
                     .arg(config_.processingWidth)
                     .arg(config_.processingHeight);
    }

    return chain + QStringLiteral(" ! d3d11download");
}

/**
 * @brief  프레임을 시스템 메모리로 내려받아야 하는 구성인지 판단합니다.
 * @return CPU에서 픽셀을 만져야 하면 true
 *
 * @details 블러도 videobalance/gamma도 CPU 요소라 프레임이 시스템 메모리에 있어야 한다.
 *          둘 다 필요 없으면 디코딩된 텍스처를 그대로 sink에 넘길 수 있고, 그러면 채널당
 *          매 프레임 일어나던 GPU->CPU 다운로드와 sink에서의 재업로드가 통째로 사라진다.
 *          d3d11download는 staging 텍스처로 복사한 뒤 Map(READ)로 GPU를 기다리므로,
 *          비용이 전송량뿐 아니라 프레임마다 GPU 큐를 비우는 동기화 지점이라는 점이 더 크다.
 *          UI가 같은 iGPU를 쓰는 동안 그 대기가 길어지면서 영상 끊김으로 나타난다.
 */
bool GstRtspReceiver::needsSystemMemoryChain() const {
    if (blurProcessor_.hasEnabledTargets()) {
        return true;
    }

    const VideoPreprocessingSettings& preprocessing = config_.preprocessing;
    return preprocessing.enabled &&
           (preprocessing.brightness != 0 || preprocessing.contrast != 1.0 || preprocessing.gamma != 1.0);
}

/**
 * @brief                     영상 체인 bin description을 만듭니다.
 * @param systemMemoryChain   true면 CPU 처리 경로, false면 GPU 전용 경로
 * @return                    gst_parse_bin_from_description에 넘길 문자열
 *
 * @details 블러 정렬용 지연은 **sink의 ts-offset**으로 만든다. 예전에는 별도 alignmentqueue에
 *          min-threshold-time을 걸었는데, queue 문서상 그 값은 "출력을 허용하는 최소 보유량"이라
 *          임계값 아래로 내려가면 출력이 다시 멈춘다. 즉 쌓아 둔 분량을 언더런 흡수에 쓸 수 없어
 *          '지연'이기만 하고 '완충'이 아니었고, 짧은 언더런도 임계값을 다시 채울 때까지 늘어났다.
 *          sink가 클럭에 맞춰 꺼내가면 같은 지연이 renderqueue의 진짜 여유분이 된다.
 *
 *          그래서 세 값이 한 몸이다. **셋을 따로 만지지 마라.**
 *            sinkSync=true : ts-offset은 clock 동기화 경로에서만 쓰인다. false면 지연이 사라진다
 *                            (설정 로더가 막는다).
 *            ts-offset     : alignmentDelayMs. 블러가 없는 GPU 경로에는 정렬할 대상이 없어 0이다.
 *            renderqueue   : ts-offset만큼을 담아야 한다. 못 담으면 leaky=downstream이 상시 프레임을
 *                            버려서 지연이 서지 않는다. 그래서 상한이 여유분 + 지연이다.
 *
 *          max-lateness는 -1로 끈다. 요소 기본값이 5 ms라 sync=true로 켜는 순간 조금만 늦은 프레임도
 *          sink가 버리는데, 드롭 지점은 renderqueue 하나로 유지하는 편이 원인을 읽기 쉽다.
 *
 *          queue 상한은 전부 시간으로만 건다. buffer 개수 상한은 같은 시간이라도 fps에 따라 값이
 *          달라져서, 30fps에서 지연보다 먼저 걸리면 위의 관계가 조용히 무너진다.
 *
 *          navigation 이벤트는 끈다(요소 기본값 true). 앱은 GstNavigation을 쓰지 않는데,
 *          켜 두면 영상 위에서 마우스가 움직일 때마다 sink가 파이프라인 상류 전체로 이벤트를 올린다.
 *
 *          포맷은 디코더가 내는 NV12를 끝까지 유지한다. videobalance/gamma/qtblur/d3d11videosink가
 *          모두 NV12를 받으므로 videoconvert는 d3d11 경로에서 통과만 하고, BGRA로 바꿀 때 들던
 *          픽셀당 4바이트 풀프레임 변환과 그만큼 늘어난 GPU 왕복 전송이 사라진다.
 */
QString GstRtspReceiver::videoChainDescription(bool systemMemoryChain) const {
    const qint64 playoutDelayMsec = systemMemoryChain ? config_.alignmentDelayMsec : 0;

    // decodequeue는 압축 H.264를 담으므로 깊어도 싸다(4 Mbps 기준 1초 = 약 500 KB). 여기서 막히면
    // 그 backpressure가 rtpjitterbuffer까지 올라가고, drop-on-latency=true가 초과분을 버려서
    // 로컬 GPU 히컵이 네트워크 손실처럼 나타난다(depay의 wait-for-keyframe 때문에 다음 IDR까지 정지).
    // leaky는 절대 켜지 마라. 디코더 앞에서 프레임을 버리면 화면이 깨진다
    const QString commonHead = QStringLiteral(
                                   "rtph264depay name=depay request-keyframe=true "
                                   "wait-for-keyframe=true ! "
                                   "h264parse config-interval=-1 ! "
                                   "queue name=decodequeue silent=true max-size-buffers=0 "
                                   "max-size-bytes=0 max-size-time=%1 ! "
                                   "valve name=presentationvalve drop=false ! "
                                   "%2 ! identity name=framewatch silent=true signal-handoffs=false ! ")
                                   .arg(config_.decodeQueueMaximumTimeMsec * 1000LL * 1000LL)
                                   .arg(decoderChain(systemMemoryChain));

    const QString renderQueue = QStringLiteral(
                                    "queue name=renderqueue silent=true leaky=downstream "
                                    "max-size-buffers=0 max-size-bytes=0 "
                                    "max-size-time=%1 ! ")
                                    .arg((config_.renderQueueMaximumTimeMsec + playoutDelayMsec) * 1000LL * 1000LL);

    // 값을 직접 끼워 넣는다. 예전처럼 기본값 문자열을 만들어 두고 replace로 갈아끼우면
    // "sync=false"가 "async=false" 안쪽에도 걸려서, sinkSync를 켜는 순간 async까지 같이 켜지고
    // 그 뒤 async 치환은 대상을 못 찾아 조용히 무시된다
    const auto boolText = [](bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); };
    const QString sink = QStringLiteral(
                             "d3d11videosink name=videosink force-aspect-ratio=true "
                             "enable-last-sample=false enable-navigation-events=false "
                             "max-lateness=-1 ts-offset=%1 qos=%2 "
                             "sync=%3 async=%4")
                             .arg(playoutDelayMsec * 1000LL * 1000LL)
                             .arg(boolText(config_.sinkQos), boolText(config_.sinkSync), boolText(config_.sinkAsync));

    QString description = commonHead;

    if (systemMemoryChain) {
        description += QStringLiteral("videoconvert ! video/x-raw,format=NV12 ! ");
        description += renderQueue;
        // 밝기·대비·감마는 qtpreprocess 하나가 룩업 한 번으로 처리한다. 예전에는 videobalance와
        // gamma 두 요소가 Y 평면을 각각 한 번씩, 모두 두 번 훑었다. 값은 그때와 완전히 같다
        description += QStringLiteral("qtpreprocess name=preprocess ! qtblur name=blur ! ");
    } else {
        description += renderQueue;
    }

    description += sink;

    return description;
}

/**
 * @brief 실행 중인 videobalance와 gamma 요소에 현재 전처리 설정을 반영합니다.
 */
void GstRtspReceiver::applyVideoPreprocessingSettings() {
    if (!pipeline_) {
        return;
    }

    GstElement* videoChain = gst_bin_get_by_name(GST_BIN(pipeline_), "videochain");
    if (!videoChain || !GST_IS_BIN(videoChain)) {
        if (videoChain) {
            gst_object_unref(videoChain);
        }
        return;
    }

    GstElement* preprocess = gst_bin_get_by_name(GST_BIN(videoChain), "preprocess");
    gst_object_unref(videoChain);

    const bool enabled = config_.preprocessing.enabled;
    if (preprocess) {
        // 표를 다시 만들고 항등이면 passthrough까지 필터가 스스로 정한다
        VideoPreprocessFilter::setSettings(preprocess, config_.preprocessing);
        gst_object_unref(preprocess);
    }
    qDebug().noquote() << QStringLiteral("[VIDEO PREPROCESS] enabled=%1 brightness=%2 contrast=%3 gamma=%4")
                              .arg(enabled)
                              .arg(config_.preprocessing.brightness)
                              .arg(config_.preprocessing.contrast, 0, 'f', 2)
                              .arg(config_.preprocessing.gamma, 0, 'f', 2);
}

/**
 * @brief 블러 대상이 하나도 없으면 qtblur를 passthrough로 내립니다.
 *
 * @details videobalance/gamma를 중립값에서 passthrough로 내리는 것과 같은 처리다. passthrough가
 *          아니면 GstBaseTransform이 매 프레임 버퍼를 쓰기 가능 상태로 만들어 transform_ip을
 *          호출하고, 버퍼가 쓰기 불가능하면 프레임 전체를 복사한다(always_in_place=TRUE).
 *          블러를 꺼 둔 채널에서는 그 비용이 전부 낭비다.
 */
void GstRtspReceiver::applyBlurPassthrough() {
    if (!pipeline_) {
        return;
    }

    GstElement* videoChain = gst_bin_get_by_name(GST_BIN(pipeline_), "videochain");
    if (!videoChain || !GST_IS_BIN(videoChain)) {
        if (videoChain) {
            gst_object_unref(videoChain);
        }
        return;
    }

    GstElement* blur = gst_bin_get_by_name(GST_BIN(videoChain), "blur");
    gst_object_unref(videoChain);
    if (!blur) {
        return;
    }

    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(blur), !blurProcessor_.hasEnabledTargets());
    gst_object_unref(blur);
}

/**
 * @brief 디코더 앞 valve에 현재 구역 표시 상태를 반영합니다.
 *
 * 숨긴 구역은 RTSP/RTP 수신과 H.264 depay/parse만 유지하고 디코딩과 화면 출력을
 * 건너뜁니다.
 */
void GstRtspReceiver::applyPresentationState() {
    if (!pipeline_) {
        return;
    }

    GstElement* videoChain = gst_bin_get_by_name(GST_BIN(pipeline_), "videochain");
    if (!videoChain || !GST_IS_BIN(videoChain)) {
        if (videoChain) {
            gst_object_unref(videoChain);
        }
        return;
    }

    GstElement* valve = gst_bin_get_by_name(GST_BIN(videoChain), "presentationvalve");
    gst_object_unref(videoChain);
    if (!valve) {
        qWarning() << "[GstRtspReceiver] Failed to find presentation valve";
        return;
    }

    g_object_set(valve, "drop", presentationActive_ ? FALSE : TRUE, nullptr);
    gst_object_unref(valve);
}

/**
 * @brief   GStreamer bus message를 주기적으로 처리하고 오류/상태/timeout에
 * 대응합니다.
 */
void GstRtspReceiver::pollBus() {
    if (!pipeline_) {
        return;
    }

    GstBus* bus = gst_element_get_bus(pipeline_);

    if (!bus) {
        return;
    }

    GstMessage* msg = nullptr;

    while (
        (msg = gst_bus_pop_filtered(
             bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS |
                                              GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_LATENCY |
                                              GST_MESSAGE_CLOCK_LOST | GST_MESSAGE_ELEMENT))) != nullptr) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* debugInfo = nullptr;

                gst_message_parse_error(msg, &err, &debugInfo);

                const QString errorText = normalizedGstErrorText(err, debugInfo);

                qDebug().noquote() << "[GStreamer Error]" << errorText;
                emit errorOccurred(errorText);

                if (debugInfo) {
                    qDebug().noquote() << "[GStreamer Debug]" << QString::fromUtf8(debugInfo);
                    g_free(debugInfo);
                }

                if (err) {
                    g_error_free(err);
                }

                gst_message_unref(msg);
                gst_object_unref(bus);

                teardownPipeline();
                scheduleReconnect(errorText, isAuthenticationFailure(errorText)
                                                 ? config_.authenticationFailureReconnectDelayMsec
                                                 : 0);
                return;
            }

            case GST_MESSAGE_WARNING: {
                GError* warning = nullptr;
                gchar* debugInfo = nullptr;
                gst_message_parse_warning(msg, &warning, &debugInfo);

                qWarning().noquote() << "[GStreamer Warning]"
                                     << (warning ? QString::fromUtf8(warning->message)
                                                 : QStringLiteral("Unknown GStreamer warning"));

                if (debugInfo) {
                    qDebug().noquote() << "[GStreamer Warning Debug]" << QString::fromUtf8(debugInfo);
                    g_free(debugInfo);
                }

                if (warning) {
                    g_error_free(warning);
                }
                break;
            }

            case GST_MESSAGE_EOS:
                qDebug().noquote() << "[GStreamer] End of stream";
                emit statusChanged("End of stream");
                gst_message_unref(msg);
                gst_object_unref(bus);
                teardownPipeline();
                scheduleReconnect("end of stream");
                return;

            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                    GstState oldState;
                    GstState newState;
                    GstState pendingState;

                    gst_message_parse_state_changed(msg, &oldState, &newState, &pendingState);

                    const QString stateText = QString("State: %1").arg(gst_element_state_get_name(newState));

                    qDebug().noquote() << "[GStreamer State]" << stateText;
                    emit statusChanged(stateText);

                    // PLAYING은 파이프라인 상태 전환만 의미하므로 실제 프레임 수신 뒤에
                    // 재연결 카운터를 초기화합니다.
                }
                break;

            case GST_MESSAGE_ASYNC_DONE:
                if (!firstAsyncDoneReported_) {
                    firstAsyncDoneReported_ = true;
                    emit statusChanged(QString("First async done in %1 ms").arg(startupTimer_.elapsed()));
                }
                break;

            case GST_MESSAGE_LATENCY:
                gst_bin_recalculate_latency(GST_BIN(pipeline_));
                break;

            case GST_MESSAGE_CLOCK_LOST:
                qWarning() << "[GStreamer] Pipeline clock lost; selecting a new clock";
                gst_element_set_state(pipeline_, GST_STATE_PAUSED);
                gst_element_set_state(pipeline_, GST_STATE_PLAYING);
                break;

            case GST_MESSAGE_ELEMENT: {
                const GstStructure* structure = gst_message_get_structure(msg);

                if (structure && gst_structure_has_name(structure, "GstRTSPSrcTimeout")) {
                    gchar* detail = gst_structure_to_string(structure);
                    const QString timeoutDetail = detail ? QString::fromUtf8(detail) : QStringLiteral("unknown");
                    const QString reason = QString("RTSP transport timeout: %1").arg(timeoutDetail);

                    qDebug().noquote() << "[GStreamer RTSP Timeout]" << reason;
                    emit statusChanged(reason);

                    if (detail) {
                        g_free(detail);
                    }

                    // rtspsrc가 UDP RTP timeout 후 TCP fallback을 완료할 수 있도록 세션을
                    // 유지합니다.
                }

                break;
            }

            default:
                break;
        }

        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    checkStall();
}
