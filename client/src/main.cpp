#include <gst/gst.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <memory>
#include <utility>

#include "auth/LocalFileAuthGateway.h"
#include "config/ApplicationConfig.h"
#include "network/gateways/MqttDeviceStatusGatewayFactory.h"
#include "network/gateways/SlackReportGateway.h"
#include "ui/dialogs/LoginWindow.h"
#include "ui/mainwindow.h"
#include "ui/panels/DefaultDashboardPanelFactory.h"
#include "video/GstStreamReceiverFactory.h"

namespace {
/**
 * @brief GStreamer MinGW 런타임에서 GIO 모듈을 분리한다.
 *
 * @details Qt MinGW와 GStreamer 런타임의 GIO 모듈 충돌을 방지한다.
 */
void configureGstreamerMinGwRuntime() {
#ifdef Q_OS_WIN
    const QString moduleDirectory = QDir(QDir::tempPath()).filePath(QStringLiteral("Qtcctvclient/gio-modules"));
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
 * @brief 애플리케이션 QSS를 로드한다.
 * @param app 스타일을 적용할 QApplication
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
 * @brief 애플리케이션 진입점이다.
 * @param argc 명령행 인자 개수
 * @param argv 명령행 인자 목록
 * @return Qt 이벤트 루프 종료 코드
 */
int main(int argc, char* argv[]) {
    int ret = 0;

    {
#ifdef Q_OS_WIN
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
#endif
        QApplication app(argc, argv);
        app.setWindowIcon(QIcon(QStringLiteral(":/icons/main.png")));

        const ApplicationConfigLoadResult configResult = ApplicationConfigLoader::load();
        if (!configResult.successful) {
            qCritical().noquote() << QStringLiteral("[Config] %1").arg(configResult.error);
            return 1;
        }

        if (!configResult.config.logging.enabled) {
            QLoggingCategory::setFilterRules(
                QStringLiteral("*.debug=false\n*.info=false\n*.warning=true\n*.critical=true"));
        }

        configureGstreamerMinGwRuntime();
        gst_init(&argc, &argv);
        loadApplicationStyle(app);

        qInfo().noquote() << QStringLiteral("[Config] Loaded %1").arg(configResult.sourcePath);

        // 폴더째 옮겨 쓰는 배포에서 계정을 만들어 두기 위한 provisioning 경로.
        // 비밀번호가 명령줄에 남으므로(프로세스 목록에서 보임) 현장 운영용이 아니라
        // 배포 준비용이다. 최초 실행 시 화면에서 만드는 흐름으로 대체하는 것이 정석이다
        const QStringList arguments = app.arguments();
        const qsizetype createUserIndex = arguments.indexOf(QStringLiteral("--create-user"));
        if (createUserIndex >= 0) {
            if (arguments.size() < createUserIndex + 4) {
                qCritical().noquote() << QStringLiteral("usage: --create-user <name> <password> <admin|operator>");
                return 2;
            }

            LocalFileAuthGateway gateway(LocalFileAuthGateway::usersFilePathFor(configResult.sourcePath));
            QString createError;
            if (!gateway.createUser(arguments.at(createUserIndex + 1), arguments.at(createUserIndex + 2),
                                    arguments.at(createUserIndex + 3), createError)) {
                qCritical().noquote() << QStringLiteral("[Login] %1").arg(createError);
                return 2;
            }

            qInfo().noquote() << QStringLiteral("[Login] Created %1 (%2)")
                                     .arg(arguments.at(createUserIndex + 1), arguments.at(createUserIndex + 3));
            return 0;
        }

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
                              configResult.config.digitalTwin, configResult.sourcePath);

            window.setWindowIcon(app.windowIcon());

            // 로그인 창은 MainWindow와 같은 깊이의 최상위 창으로 덮는다. 자식 위젯으로
            // 덮으면 MainWindow의 QQuickWidget이 통째로 사라진다.
            //
            // 창을 띄우기 전에 먼저 만든다. 생성자의 QML 로드가 수백 ms 걸리는데, 그 사이
            // MainWindow가 이미 화면에 올라와 있으면 대시보드가 그대로 드러난다. 여기서
            // 만들어 두면 show 두 번이 같은 event loop turn에 붙어 노출 구간이 없어진다
            auto authGateway =
                std::make_shared<LocalFileAuthGateway>(LocalFileAuthGateway::usersFilePathFor(configResult.sourcePath));
            LoginWindow login(authGateway, &window);
            if (!login.isReady()) {
                qCritical() << "[Login] Login screen failed to load";
                return 1;
            }

            QObject::connect(&login, &LoginWindow::authenticated, &window, &MainWindow::beginSession);
            QObject::connect(&login, &LoginWindow::fullScreenToggleRequested, &window, &MainWindow::toggleFullScreen);

            // 로그인 창은 메인 창을 부모로 둔 Qt::Dialog다. Windows는 "소유된 창"을 언제나
            // 소유자 위에 유지하는데, 그 소유 관계는 **부모의 네이티브 창이 이미 있을 때만**
            // 맺어진다. 여기서 미리 만들어 두지 않으면 로그인 창이 주인 없는 창으로 떠서,
            // 뒤에 showMaximized()가 대시보드를 Z-order 맨 위로 올리는 순간 한 프레임 드러난다
            window.createWinId();

            // 메인 창이 focus를 가져가지 않게 한다. focus는 로그인 창이 사라질 때 넘긴다
            window.setAttribute(Qt::WA_ShowWithoutActivating, true);

            // 로그인 화면을 먼저 띄운다. 메인 창을 먼저 보이면 패널이 다 그려지기 전의
            // 반쯤 칠해진 대시보드가 그대로 노출된다
            login.setGeometry(QGuiApplication::primaryScreen()->availableGeometry());
            login.show();
            login.raise();
            login.activateWindow();

            // 로그인 화면이 한 번 그려진 뒤에 메인 창을 그 아래로 넣는다. 여기서 메인 창을
            // 띄우면 Windows가 Z-order 맨 위에 올리므로 같은 호출 안에서 되돌린다
            // (사이에 화면 갱신이 없어 눈에 보이지 않는다)
            bool mainWindowShown = false;
            const auto showMainWindow = [&]() {
                if (mainWindowShown) {
                    return;
                }
                mainWindowShown = true;

                // 설정 크기는 최대화를 풀었을 때 돌아갈 크기로 남기고, 시작은 창 모드 최대화로
                // 연다 (전체 화면이 아니라 제목 표시줄이 있는 최대화다. 전체 화면은 Alt+Enter)
                window.resize(configResult.config.window.width, configResult.config.window.height);
                window.showMaximized();

                login.coverWidget(&window);
                login.raise();
                login.activateWindow();
            };

            QObject::connect(&login, &LoginWindow::firstFrameRendered, &window, showMainWindow);
            // 렌더 신호가 오지 않는 환경에서도 메인 창 없이 멈추지 않게 한다
            QTimer::singleShot(3000, &window, showMainWindow);

            ret = app.exec();
        }
    }

    gst_deinit();

    return ret;
}
