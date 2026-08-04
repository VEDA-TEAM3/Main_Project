#include <gst/gst.h>

#include <cstdio>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QtGlobal>
#include <memory>
#include <utility>

#include "config/ApplicationConfig.h"
#include "network/gateways/MqttDeviceStatusGatewayFactory.h"
#include "network/gateways/SlackReportGateway.h"
#include "ui/mainwindow.h"
#include "ui/panels/DefaultDashboardPanelFactory.h"
#include "video/GstStreamReceiverFactory.h"

namespace {
QFile applicationLogFile;
QMutex applicationLogMutex;

/**
 * @brief       Qt 로그를 파일과 stderr에 줄 단위로 온전히 기록합니다.
 * @param type  메시지 종류
 * @param text  메시지 본문
 *
 * @details Windows GUI 서브시스템 실행 파일에는 콘솔이 없어 Qt 기본 핸들러가 메시지를
 *          OutputDebugString으로 보낸다. 이 경로는 디버거의 공유 버퍼를 거치므로 긴 줄이
 *          잘리고, 여러 스레드(MQTT 게이트웨이, RTSP 수신기, GUI)가 동시에 찍으면 줄이 섞인다.
 *          진단 로그는 그 스레드들에서 나오므로 뮤텍스로 직렬화해 파일에 직접 쓴다.
 *          ponytail: 줄마다 flush한다. 크래시 직전 줄까지 남기는 대신 초당 수백 줄 이상에서는
 *          느려지므로, 그 분량이 필요해지면 버퍼링 후 주기적 flush로 바꾼다.
 */
void writeApplicationLog(QtMsgType type, const QMessageLogContext& /*context*/, const QString& text) {
    const QString prefix = type == QtWarningMsg    ? QStringLiteral("[W] ")
                           : type == QtCriticalMsg ? QStringLiteral("[C] ")
                           : type == QtFatalMsg    ? QStringLiteral("[F] ")
                                                   : QString();
    const QByteArray line = (prefix + text + QLatin1Char('\n')).toUtf8();

    const QMutexLocker locker(&applicationLogMutex);
    applicationLogFile.write(line);
    applicationLogFile.flush();
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
}

/** @brief VEDA_LOG_FILE이 지정되면 그 파일로 로그를 받는 메시지 핸들러를 설치합니다. */
void installFileLogging() {
    const QString path = qEnvironmentVariable("VEDA_LOG_FILE").trimmed();
    if (path.isEmpty()) {
        return;
    }

    applicationLogFile.setFileName(path);
    if (!applicationLogFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << QStringLiteral("[Log] Failed to open %1").arg(path);
        return;
    }

    qInstallMessageHandler(writeApplicationLog);
    qInfo().noquote() << QStringLiteral("[Log] Writing to %1").arg(path);
}

/**
 * @brief GStreamer MinGW 런타임에서 선택형 GIO 프록시 모듈을 분리합니다.
 *
 * Qt MinGW와 GStreamer 번들의 서로 다른 C++ 런타임이 libgiolibproxy.dll에서
 * 충돌할 수 있으므로, RTSP 수신에 필요하지 않은 GIO 확장 모듈 탐색을 비활성화합니다.
 */
void configureGstreamerMinGwRuntime() {
#ifdef Q_OS_WIN
    const QString moduleDirectory = QDir(QDir::tempPath()).filePath(QStringLiteral("QtDemo/gio-modules"));
    if (!QDir().mkpath(moduleDirectory)) {
        qWarning().noquote()
            << QStringLiteral("[GStreamer] Failed to create isolated GIO module directory: %1").arg(moduleDirectory);
        return;
    }

    const QByteArray nativeDirectory = QFile::encodeName(moduleDirectory);
    if (!g_setenv("GIO_MODULE_DIR", nativeDirectory.constData(), true)) {
        qWarning().noquote() << QStringLiteral("[GStreamer] Failed to configure isolated GIO module directory");
        return;
    }

    g_unsetenv("GIO_EXTRA_MODULES");
    qInfo().noquote()
        << QStringLiteral("[GStreamer] Using MinGW runtime with isolated GIO modules: %1").arg(moduleDirectory);
#endif
}

/**
 * @brief      애플리케이션 QSS를 로드합니다.
 * @param app  스타일을 적용할 QApplication
 */
void loadApplicationStyle(QApplication& app) {
    const QStringList stylePaths = {QStringLiteral(":/styles/app.qss")};

    for (const QString& path : stylePaths) {
        QFile styleFile(path);

        if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
            app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
            qDebug().noquote() << "[Style] Loaded" << path;
            return;
        }
    }

    qWarning() << "[Style] Failed to load app.qss";
}
}  // namespace

/**
 * @brief       애플리케이션 진입점입니다.
 * @param argc  명령행 인자 개수
 * @param argv  명령행 인자 목록
 * @return      Qt 이벤트 루프 종료 코드
 */
int main(int argc, char* argv[]) {
    installFileLogging();
    configureGstreamerMinGwRuntime();
    gst_init(&argc, &argv);

    int ret = 0;

    {
        QApplication app(argc, argv);
        app.setWindowIcon(QIcon(QStringLiteral(":/icons/main.png")));
        loadApplicationStyle(app);

        const ApplicationConfigLoadResult configResult = ApplicationConfigLoader::load();
        if (!configResult.successful) {
            qCritical().noquote() << QStringLiteral("[Config] %1").arg(configResult.error);
            gst_deinit();
            return 1;
        }
        qInfo().noquote() << QStringLiteral("[Config] Loaded %1").arg(configResult.sourcePath);

        {
            auto streamReceiverFactory = std::make_shared<GstStreamReceiverFactory>(configResult.config.video.receiver);
            auto deviceStatusGatewayFactory =
                std::make_shared<MqttDeviceStatusGatewayFactory>(configResult.config.mqtt);
            auto dashboardPanelFactory = std::make_shared<DefaultDashboardPanelFactory>();
            auto reportGateway = std::make_shared<SlackReportGateway>(
                qEnvironmentVariable("SLACK_BOT_TOKEN"), qEnvironmentVariable("SLACK_REPORT_TARGET"),
                qEnvironmentVariable("SLACK_REPORT_USER_ID"), qEnvironmentVariable("SLACK_REPORT_CHANNEL_ID"));
            MainWindow window(std::move(streamReceiverFactory), std::move(deviceStatusGatewayFactory),
                              std::move(dashboardPanelFactory), std::move(reportGateway), configResult.config.video,
                              configResult.config.digitalTwin);

            window.setWindowIcon(app.windowIcon());
            window.resize(configResult.config.window.width, configResult.config.window.height);
            window.show();

            ret = app.exec();
        }
    }

    gst_deinit();

    return ret;
}
