#pragma once

#include <QString>
#include <QWidget>
#include <memory>

#include "auth/AuthGateway.h"

class QQuickWidget;

/**
 * @brief 대시보드를 덮는 로그인 창.
 *
 * @details MainWindow의 자식 위젯으로 화면을 덮으면 Qt가 텍스처 합성을 건너뛰어 그 창의
 *          QQuickWidget이 전부 사라집니다. 그래서 MapSettingsDialog와 같은 깊이의
 *          최상위 창(Qt::Dialog | FramelessWindowHint)으로 띄웁니다. 이 창 위에 또 다른
 *          QML 최상위 창을 올리면 검은 상자가 되므로, 2차 화면은 LoginScreen.qml 안의
 *          오버레이로 그려야 합니다.
 */
class LoginWindow final : public QWidget {
    Q_OBJECT

public:
    explicit LoginWindow(std::shared_ptr<AuthGateway> authGateway, QWidget* parent = nullptr);

    /// QML이 정상적으로 올라왔는지. false면 로그인 창을 띄울 수 없습니다
    bool isReady() const { return loginView_ != nullptr; }

    /// 덮을 대상 창의 화면 좌표에 맞춥니다
    void coverWidget(const QWidget* target);

signals:
    /// 인증 성공 즉시 발생합니다. 퇴장 연출은 이 뒤에 이어집니다
    void authenticated(const QString& userName, const QString& role);
    /// 로그인 화면에서도 Alt+Enter가 동작하도록 메인 창에 전달합니다
    void fullScreenToggleRequested();
    /// 로그인 화면이 실제로 한 번 그려졌습니다. 메인 창은 이 뒤에 띄웁니다
    void firstFrameRendered();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // QML 루트 signal은 동적 metaobject에만 있어 문자열로 연결하므로 진짜 slot이어야 한다
    void handleLoginRequested();
    void handleBootstrapRequested();
    void handleExitFinished();

private:
    void completeSignIn(const AuthenticatedUser& user);
    void setQmlValue(const char* name, const QVariant& value);
    QString qmlText(const char* name) const;

    std::shared_ptr<AuthGateway> authGateway_;
    QQuickWidget* loginView_ = nullptr;
    bool firstFrameRendered_ = false;
    /// 인증을 마쳤는지. 이 값이 false인 채로 창이 닫히면 프로그램을 끝낸다
    bool signedIn_ = false;
};
