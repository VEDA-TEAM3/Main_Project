#pragma once

#include <QtGui/qwindowdefs.h>

#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <memory>

#include "model/MqttRealtimeData.h"
#include "model/StreamConfig.h"

class QThread;
class BlurFrameBuffer;
class StreamReceiver;
class StreamReceiverFactory;

/**
 * @brief 하나의 영상 출력 창과 RTSP 스트림 설정을 연결합니다.
 *
 * MainWindow가 네이티브 영상 창의 WId를 준비한 뒤
 * StreamSessionManager에 전달할 때 사용합니다.
 */
struct StreamOutputBinding {
    StreamConfig config;
    WId outputWindowHandle = 0;
};

/**
 * @brief 여러 RTSP 수신기와 worker thread의 생명주기를 관리합니다.
 *
 * StreamReceiverFactory를 통해 채널별 수신기를 생성하고,
 * 각 수신기를 전용 QThread에서 실행합니다.
 *
 * 이 클래스는 QWidget을 직접 조작하지 않으며,
 * 영상 출력 상태는 channelIndex가 포함된 signal로 전달합니다.
 */
class StreamSessionManager final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief                  스트림 세션 관리자를 생성합니다.
     * @param receiverFactory  채널별 StreamReceiver 생성 factory
     * @param parent           Qt 객체 소유권을 연결할 부모 객체
     */
    explicit StreamSessionManager(std::shared_ptr<StreamReceiverFactory> receiverFactory, int receiverStartSpacingMsec,
                                  QObject* parent = nullptr);

    /**
     * @brief 실행 중인 모든 수신기와 worker thread를 정리합니다.
     */
    ~StreamSessionManager() override;

    /**
     * @brief          출력 창과 스트림 설정을 등록합니다.
     * @param bindings 채널별 StreamConfig와 출력 WId 목록
     *
     * 실행 중인 상태에서는 기존 세션을 정지한 뒤 다시 구성합니다.
     */
    void configure(QVector<StreamOutputBinding> bindings);

    /**
     * @brief 구성된 수신기들을 현재 시작 정책에 따라 실행합니다.
     */
    void start();

    /**
     * @brief 모든 수신기를 정지하고 worker thread를 종료합니다.
     */
    void stop();

    /** 해당 채널 receiver에 최신 blur 좌표를 전달합니다. */
    void submitBlurFrame(BlurFrameData frame);

    /** 모든 채널에 적용할 얼굴·차량 번호판 블러 활성 상태를 설정합니다. */
    void setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled);

signals:
    /**
     * @brief              특정 채널의 로딩 상태가 변경됐음을 알립니다.
     * @param channelIndex StreamConfig에 정의된 채널 인덱스
     * @param loading      현재 로딩 여부
     */
    void loadingChanged(int channelIndex, bool loading);

    /**
     * @brief              특정 채널의 상태 메시지를 전달합니다.
     * @param channelIndex StreamConfig에 정의된 채널 인덱스
     * @param status       수신기 상태 메시지
     */
    void statusChanged(int channelIndex, QString status);

    /**
     * @brief              특정 채널에서 오류가 발생했음을 알립니다.
     * @param channelIndex StreamConfig에 정의된 채널 인덱스
     * @param error        오류 메시지
     */
    void errorOccurred(int channelIndex, QString error);

    /**
     * @brief              특정 채널에서 첫 디코딩 프레임이 수신됐음을 알립니다.
     * @param channelIndex StreamConfig에 정의된 채널 인덱스
     */
    void firstFrameReceived(int channelIndex);

private:
    struct ReceiverWorker {
        StreamConfig config;
        std::shared_ptr<QThread> thread;
        std::shared_ptr<StreamReceiver> receiver;
        std::shared_ptr<BlurFrameBuffer> blurFrameBuffer;
    };

    void createWorkers();
    void connectReceiverSignals(const ReceiverWorker& worker);
    void startReceiverSequentially(qsizetype receiverIndex);
    void stopWorkers();

    std::shared_ptr<StreamReceiverFactory> receiverFactory_;
    int receiverStartSpacingMsec_ = 0;
    bool faceBlurEnabled_ = true;
    bool licensePlateBlurEnabled_ = true;

    QVector<StreamOutputBinding> bindings_;
    QVector<ReceiverWorker> receiverWorkers_;

    bool startRequested_ = false;
};
