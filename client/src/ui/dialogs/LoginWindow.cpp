#include "ui/dialogs/LoginWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QKeySequence>
#include <QQuickItem>
#include <QQuickWidget>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <utility>

#include "auth/AuthGateway.h"
#include "ui/SharedQmlEngine.h"

/**
 * @brief              로그인 창을 구성합니다.
 * @param authGateway  아이디와 비밀번호를 확인할 인증기
 * @param parent       덮을 메인 윈도우
 */
LoginWindow::LoginWindow(std::shared_ptr<AuthGateway> authGateway, QWidget* parent)
    : QWidget(parent), authGateway_(std::move(authGateway)) {
    // 윈도우 플래그는 반드시 setSource() 이전에 정한다. 이후에 바꾸면 scene graph가 깨진다
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // 최상위 QQuickWidget에 WA_TranslucentBackground를 걸면 아무것도 렌더되지 않는다
    auto* view = new QQuickWidget(sharedQmlEngine(), this);
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view->setSource(QUrl(QStringLiteral("qrc:/qml/LoginScreen.qml")));
    if (!view->rootObject()) {
        qWarning() << "[LoginWindow] QML load failed" << view->errors();
        return;
    }

    layout->addWidget(view);
    loginView_ = view;

    // QML 루트의 signal은 동적 metaobject에만 있으므로 문자열로 연결한다
    connect(loginView_->rootObject(), SIGNAL(loginRequested()), this, SLOT(handleLoginRequested()));
    connect(loginView_->rootObject(), SIGNAL(bootstrapRequested()), this, SLOT(handleBootstrapRequested()));
    connect(loginView_->rootObject(), SIGNAL(exitFinished()), this, SLOT(handleExitFinished()));

    // 계정이 없는 설치본은 로그인 대신 최초 관리자 생성으로 연다
    setQmlValue("bootstrapMode", authGateway_ && authGateway_->needsBootstrap());

    // 메인 창의 Alt+Enter는 modal인 이 창이 focus를 쥐고 있는 동안 오지 않는다.
    // 로그인 전에도 전체 화면으로 바꿀 수 있어야 하므로 같은 단축키를 여기에도 건다
    for (const QKeySequence& sequence :
         {QKeySequence(Qt::ALT | Qt::Key_Return), QKeySequence(Qt::ALT | Qt::Key_Enter)}) {
        connect(new QShortcut(sequence, this), &QShortcut::activated, this, &LoginWindow::fullScreenToggleRequested);
    }

    // 메인 창이 움직이거나 크기가 바뀌면 따라간다. 안 따라가면 로그인 화면만 제자리에 남아
    // 대시보드가 옆으로 드러난다
    if (parent) {
        parent->installEventFilter(this);
    }
}

/**
 * @brief   덮고 있는 창의 이동·크기 변화를 따라갑니다.
 *
 * @details 창 상태 변경(전체 화면 등)은 실제 크기가 반영된 뒤에 읽어야 하므로 한 프레임 미룹니다.
 */
bool LoginWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched != parentWidget()) {
        return QWidget::eventFilter(watched, event);
    }

    // 로그인 전에 메인 창이 닫히면 로그인 화면만 남는다. 그대로 프로그램을 끝낸다
    if (event->type() == QEvent::Close && !signedIn_) {
        QCoreApplication::quit();
        return QWidget::eventFilter(watched, event);
    }

    const QEvent::Type type = event->type();
    const bool geometryChanged = type == QEvent::Move || type == QEvent::Resize || type == QEvent::WindowStateChange;
    if (geometryChanged && isVisible()) {
        QTimer::singleShot(0, this, [this]() { coverWidget(parentWidget()); });
    }

    return QWidget::eventFilter(watched, event);
}

/**
 * @brief   인증 전에 로그인 창을 닫으면 프로그램을 끝냅니다.
 *
 * @details Alt+F4로 로그인 창만 닫으면 뒤의 대시보드가 그대로 드러나 관문을 통째로 건너뜁니다.
 *          닫기는 "로그인 취소"가 아니라 "프로그램 종료"여야 합니다.
 */
void LoginWindow::closeEvent(QCloseEvent* event) {
    if (!signedIn_) {
        QCoreApplication::quit();
    }
    QWidget::closeEvent(event);
}

/**
 * @brief         덮을 창의 화면 좌표와 크기에 맞춥니다.
 * @param target  가릴 대상 (보통 MainWindow)
 */
void LoginWindow::coverWidget(const QWidget* target) {
    if (!target) {
        return;
    }
    setGeometry(QRect(target->mapToGlobal(QPoint(0, 0)), target->size()));
}

/** @brief QML이 올린 로그인 요청을 인증기로 넘깁니다. */
void LoginWindow::handleLoginRequested() {
    if (!loginView_ || !authGateway_) {
        return;
    }

    setQmlValue("errorText", QString());
    setQmlValue("busy", true);

    // PBKDF2는 GUI thread를 200ms 안팎으로 잡는다. 한 프레임 뒤로 미뤄야 "확인 중..."이
    // 실제로 그려진 다음에 계산이 시작된다
    QTimer::singleShot(0, this, [this]() {
        const AuthResult result = authGateway_->authenticate(qmlText("userName"), qmlText("password"));
        setQmlValue("busy", false);

        if (!result.successful) {
            setQmlValue("password", QString());
            setQmlValue("errorText", result.error);
            return;
        }

        completeSignIn(result.user);
    });
}

/** @brief 계정이 없는 설치본에서 첫 관리자를 만들고 그대로 이어서 들어갑니다. */
void LoginWindow::handleBootstrapRequested() {
    if (!loginView_ || !authGateway_) {
        return;
    }

    setQmlValue("errorText", QString());
    setQmlValue("busy", true);

    QTimer::singleShot(0, this, [this]() {
        const QString userName = qmlText("userName").trimmed();
        QString error;
        if (!authGateway_->createInitialAdmin(userName, qmlText("password"), error)) {
            setQmlValue("busy", false);
            setQmlValue("errorText", error);
            return;
        }

        // 방금 두 번 입력한 비밀번호를 세 번째로 받지 않는다. 만든 계정으로 바로 확인한다
        const AuthResult result = authGateway_->authenticate(userName, qmlText("password"));
        setQmlValue("busy", false);
        if (!result.successful) {
            setQmlValue("errorText", result.error);
            return;
        }

        qInfo().noquote() << QStringLiteral("[Login] Created the first administrator: %1").arg(userName);
        completeSignIn(result.user);
    });
}

/** @brief 인증이 끝난 뒤 세션 시작을 알리고 퇴장 연출을 시작합니다. */
void LoginWindow::completeSignIn(const AuthenticatedUser& user) {
    signedIn_ = true;

    // 이 창은 파괴하지 않고 감추기만 하므로, 비워 두지 않으면 프로그램이 끝날 때까지
    // 비밀번호가 입력 칸에 그대로 남는다
    setQmlValue("password", QString());
    setQmlValue("passwordConfirm", QString());

    // 연출보다 먼저 알린다. 그래야 RTSP·MQTT 접속이 퇴장 애니메이션과 겹쳐 진행된다
    emit authenticated(user.name, user.role);
    QMetaObject::invokeMethod(loginView_->rootObject(), "playExit");
}

/**
 * @brief QML 퇴장이 끝나면 창을 걷어냅니다.
 *
 * @details 창 자체는 애니메이션하지 않습니다. windowOpacity를 움직이면 최상위 QQuickWidget
 *          창에 layered window 속성이 걸리면서 heap이 깨지고(0xC0000374), 위치를 움직여도
 *          같은 자리에서 access violation(0xC0000005)으로 죽습니다. 둘 다 실측했습니다.
 *          그래서 전환 연출은 전부 LoginScreen.qml 안에서 끝내고, 여기서는 이미 검게 덮인
 *          화면을 감추기만 합니다.
 *
 *          close()가 아니라 hide()입니다. close()는 마지막 창 닫힘 판정을 타서 앱이 종료됩니다.
 */
void LoginWindow::handleExitFinished() { hide(); }

void LoginWindow::setQmlValue(const char* name, const QVariant& value) {
    if (loginView_ && loginView_->rootObject()) {
        loginView_->rootObject()->setProperty(name, value);
    }
}

QString LoginWindow::qmlText(const char* name) const {
    if (!loginView_ || !loginView_->rootObject()) {
        return {};
    }
    return loginView_->rootObject()->property(name).toString();
}
