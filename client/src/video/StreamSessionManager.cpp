#include "video/StreamSessionManager.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <utility>

#include "network/LatestBlurFrameBuffer.h"
#include "video/StreamReceiver.h"
#include "video/StreamReceiverFactory.h"

/**
 * @brief                  스트림 세션 관리자를 생성합니다.
 * @param receiverFactory  채널별 StreamReceiver 생성 factory
 * @param parent           Qt 객체 소유권을 연결할 부모 객체
 */
StreamSessionManager::StreamSessionManager(std::shared_ptr<StreamReceiverFactory> receiverFactory,
                                           int receiverStartSpacingMsec, QObject* parent)
    : QObject(parent),
      receiverFactory_(std::move(receiverFactory)),
      receiverStartSpacingMsec_(receiverStartSpacingMsec) {}

/**
 * @brief 실행 중인 모든 수신기와 worker thread를 정리합니다.
 */
StreamSessionManager::~StreamSessionManager() { stop(); }

/**
 * @brief          출력 창과 스트림 설정을 등록합니다.
 * @param bindings 채널별 StreamConfig와 출력 WId 목록
 */
void StreamSessionManager::configure(QVector<StreamOutputBinding> bindings) {
    const bool restartAfterConfigure = startRequested_;

    stopWorkers();

    bindings_ = std::move(bindings);
    createWorkers();

    if (restartAfterConfigure) {
        start();
    }
}

/**
 * @brief 구성된 수신기들을 현재 시작 정책에 따라 실행합니다.
 */
void StreamSessionManager::start() {
    if (startRequested_) {
        return;
    }

    if (receiverWorkers_.isEmpty()) {
        createWorkers();
    }

    if (receiverWorkers_.isEmpty()) {
        qWarning() << "[StreamSessionManager] No stream receiver is configured";
        return;
    }

    startRequested_ = true;
    startReceiverSequentially(0);
}

/**
 * @brief 모든 수신기를 정지하고 worker thread를 종료합니다.
 */
void StreamSessionManager::stop() {
    if (!startRequested_ && receiverWorkers_.isEmpty()) {
        return;
    }

    stopWorkers();
}

void StreamSessionManager::submitBlurFrame(BlurFrameData frame) {
    for (const ReceiverWorker& worker : receiverWorkers_) {
        if (worker.config.channelIndex != frame.channelIndex || !worker.receiver || !worker.thread ||
            !worker.thread->isRunning()) {
            continue;
        }

        const auto receiver = worker.receiver;
        const auto frameBuffer = worker.blurFrameBuffer;
        if (!frameBuffer || !frameBuffer->submit(std::move(frame))) {
            return;
        }

        const bool invoked = QMetaObject::invokeMethod(
            receiver.get(),
            [receiver, frameBuffer]() {
                QVector<BlurFrameData> frames = frameBuffer->takeLatestFrames();
                for (BlurFrameData& pendingFrame : frames) {
                    receiver->setBlurFrame(std::move(pendingFrame));
                }
            },
            Qt::QueuedConnection);
        if (!invoked) {
            frameBuffer->cancelPendingDelivery();
            qWarning() << "[StreamSessionManager] Failed to deliver blur metadata for channel"
                       << worker.config.channelIndex;
        }
        return;
    }
}

/**
 * @brief                     모든 채널 수신기의 블러 대상 유형을 설정합니다.
 * @param faceEnabled         얼굴 블러 활성화 여부
 * @param licensePlateEnabled 차량 번호판 블러 활성화 여부
 */
void StreamSessionManager::setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) {
    faceBlurEnabled_ = faceEnabled;
    licensePlateBlurEnabled_ = licensePlateEnabled;

    for (const ReceiverWorker& worker : receiverWorkers_) {
        if (!worker.receiver || !worker.thread || !worker.thread->isRunning()) {
            continue;
        }

        const auto receiver = worker.receiver;
        QMetaObject::invokeMethod(
            receiver.get(),
            [receiver, faceEnabled, licensePlateEnabled]() {
                receiver->setBlurTargetsEnabled(faceEnabled, licensePlateEnabled);
            },
            Qt::QueuedConnection);
    }
}

/**
 * @brief 등록된 출력 정보에 맞춰 receiver와 전용 worker thread를 생성합니다.
 */
void StreamSessionManager::createWorkers() {
    if (!receiverWorkers_.isEmpty()) {
        return;
    }

    if (!receiverFactory_) {
        qWarning() << "[StreamSessionManager] Stream receiver factory is not configured";
        return;
    }

    receiverWorkers_.reserve(bindings_.size());

    for (const StreamOutputBinding& binding : bindings_) {
        const StreamConfig& config = binding.config;

        if (!config.enabled) {
            continue;
        }

        if (binding.outputWindowHandle == 0) {
            const QString errorText = QStringLiteral("Output window handle is invalid");

            qWarning().noquote() << QStringLiteral("[StreamSessionManager] %1: %2").arg(config.cameraId, errorText);

            emit errorOccurred(config.channelIndex, errorText);
            continue;
        }

        auto receiverThread = std::make_shared<QThread>();
        receiverThread->setObjectName(QStringLiteral("%1-worker").arg(config.cameraId));

        auto receiver = receiverFactory_->create(binding.outputWindowHandle);

        if (!receiver) {
            const QString errorText = QStringLiteral("Failed to create stream receiver");

            qWarning().noquote() << QStringLiteral("[StreamSessionManager] %1: %2").arg(config.cameraId, errorText);

            emit errorOccurred(config.channelIndex, errorText);
            continue;
        }

        receiver->setObjectName(config.cameraId);
        receiver->setUrl(config.url);
        receiver->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);
        receiver->moveInternalObjectsToThread(receiverThread.get());

        if (receiver->thread() != receiverThread.get()) {
            const QString errorText = QStringLiteral("Failed to move stream receiver to worker thread");

            qWarning().noquote() << QStringLiteral("[StreamSessionManager] %1: %2").arg(config.cameraId, errorText);

            emit errorOccurred(config.channelIndex, errorText);
            continue;
        }

        ReceiverWorker worker;
        worker.config = config;
        worker.thread = std::move(receiverThread);
        worker.receiver = std::move(receiver);
        worker.blurFrameBuffer = std::make_shared<LatestBlurFrameBuffer>();

        connectReceiverSignals(worker);

        worker.thread->start();
        receiverWorkers_.append(std::move(worker));
    }
}

/**
 * @brief        receiver의 상태 signal을 채널 인덱스가 포함된 manager signal로 중계합니다.
 * @param worker 연결할 receiver와 채널 설정
 */
void StreamSessionManager::connectReceiverSignals(const ReceiverWorker& worker) {
    if (!worker.receiver) {
        return;
    }

    const int channelIndex = worker.config.channelIndex;
    const QString cameraId = worker.config.cameraId;
    const QString cameraName = worker.config.name;

    connect(
        worker.receiver.get(), &StreamReceiver::loadingChanged, this,
        [this, channelIndex](bool loading) { emit loadingChanged(channelIndex, loading); }, Qt::QueuedConnection);

    connect(
        worker.receiver.get(), &StreamReceiver::statusChanged, this,
        [this, channelIndex, cameraId, cameraName](const QString& status) {
            qDebug().noquote() << QStringLiteral("[%1 Status]").arg(cameraId) << cameraName << status;
            emit statusChanged(channelIndex, status);
        },
        Qt::QueuedConnection);

    connect(
        worker.receiver.get(), &StreamReceiver::errorOccurred, this,
        [this, channelIndex, cameraId, cameraName](const QString& error) {
            qWarning().noquote() << QStringLiteral("[%1 Error]").arg(cameraId) << cameraName << error;
            emit errorOccurred(channelIndex, error);
        },
        Qt::QueuedConnection);

    connect(
        worker.receiver.get(), &StreamReceiver::firstFrameReceived, this,
        [this, channelIndex]() { emit firstFrameReceived(channelIndex); }, Qt::QueuedConnection);
}

/**
 * @brief                지정된 수신기를 시작하고 다음 수신기 시작을 예약합니다.
 * @param receiverIndex  시작할 receiverWorkers_ 인덱스
 */
void StreamSessionManager::startReceiverSequentially(qsizetype receiverIndex) {
    if (!startRequested_) {
        return;
    }

    if (receiverIndex >= receiverWorkers_.size()) {
        qDebug() << "[StreamSessionManager] All stream receivers requested";
        return;
    }

    const ReceiverWorker& worker = receiverWorkers_[receiverIndex];

    if (!worker.receiver || !worker.thread || !worker.thread->isRunning()) {
        qWarning().noquote()
            << QStringLiteral("[StreamSessionManager] Receiver is not available: %1").arg(worker.config.cameraId);

        QTimer::singleShot(0, this, [this, receiverIndex]() { startReceiverSequentially(receiverIndex + 1); });

        return;
    }

    const auto receiver = worker.receiver;

    qDebug().noquote() << QStringLiteral("[%1] start").arg(worker.config.cameraId);

    const bool invoked =
        QMetaObject::invokeMethod(receiver.get(), [receiver]() { receiver->start(); }, Qt::QueuedConnection);

    if (!invoked) {
        const QString errorText = QStringLiteral("Failed to request stream receiver start");

        qWarning().noquote() << QStringLiteral("[StreamSessionManager] %1: %2").arg(worker.config.cameraId, errorText);

        emit errorOccurred(worker.config.channelIndex, errorText);
    }

    QTimer::singleShot(receiverStartSpacingMsec_, this,
                       [this, receiverIndex]() { startReceiverSequentially(receiverIndex + 1); });
}

/**
 * @brief 실행 중인 receiver를 정지하고 모든 worker thread를 종료합니다.
 */
void StreamSessionManager::stopWorkers() {
    startRequested_ = false;

    if (receiverWorkers_.isEmpty()) {
        return;
    }

    QThread* ownerThread = thread();
    QThread* currentThread = QThread::currentThread();

    for (const ReceiverWorker& worker : receiverWorkers_) {
        const auto& receiver = worker.receiver;

        if (worker.blurFrameBuffer) {
            worker.blurFrameBuffer->clear();
        }

        if (!receiver) {
            continue;
        }

        QThread* receiverThread = receiver->thread();

        if (receiverThread && receiverThread != currentThread && receiverThread->isRunning()) {
            const bool invoked = QMetaObject::invokeMethod(
                receiver.get(),
                [receiver = receiver.get(), ownerThread]() {
                    receiver->stop();
                    receiver->moveInternalObjectsToThread(ownerThread);
                },
                Qt::BlockingQueuedConnection);

            if (!invoked) {
                qWarning().noquote()
                    << QStringLiteral("[StreamSessionManager] Failed to stop receiver: %1").arg(worker.config.cameraId);
            }

            continue;
        }

        receiver->stop();

        if (receiver->thread() == currentThread && receiver->thread() != ownerThread) {
            receiver->moveInternalObjectsToThread(ownerThread);
        }
    }

    for (const ReceiverWorker& worker : receiverWorkers_) {
        const auto& receiverThread = worker.thread;

        if (!receiverThread) {
            continue;
        }

        receiverThread->quit();
        receiverThread->wait();
    }

    receiverWorkers_.clear();
}
