#include <gst/gst.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QStringList>
#include <QtGlobal>
#include <memory>
#include <utility>

#include "config/ApplicationConfig.h"
#include "network/MqttDeviceStatusGatewayFactory.h"
#include "network/SlackReportGateway.h"
#include "ui/mainwindow.h"
#include "ui/panels/DefaultDashboardPanelFactory.h"
#include "video/GstStreamReceiverFactory.h"

namespace {
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
                              std::move(dashboardPanelFactory), std::move(reportGateway), configResult.config.video);
            window.setWindowIcon(app.windowIcon());
            window.resize(configResult.config.window.width, configResult.config.window.height);
            window.show();

            ret = app.exec();
        }
    }

    gst_deinit();

    return ret;
}
