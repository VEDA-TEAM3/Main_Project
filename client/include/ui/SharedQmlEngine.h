#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QHoverEvent>
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <QtDebug>

/**
 * @brief  앱 전체 QQuickWidget이 함께 쓰는 단일 QML 엔진을 돌려줍니다.
 * @return QCoreApplication이 소유하는 공용 엔진
 *
 * @details QQuickWidget마다 엔진을 따로 만들면 같은 창 안에서 델리게이트 파기 중 heap이 깨졌습니다.
 * 새 QQuickWidget은 반드시 이 엔진을 넘겨 생성하세요.
 */
inline QQmlEngine* sharedQmlEngine() {
    static QQmlEngine* engine = new QQmlEngine(QCoreApplication::instance());
    return engine;
}

/**
 * @brief 포인터가 QQuickWidget 밖으로 나갈 때 남아 있는 마우스 커서 모양을 되돌립니다.
 *
 * @details QML의 cursorShape는 QQuickWidget 위젯이 아니라 최상위 창(QWindow)에 걸리는데, 포인터가
 * QQuickWidget 밖으로 나가도 장면에는 leave가 전달되지 않아 손 모양이 그대로 남습니다.
 */
class QuickCursorResetFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Leave) {
            if (auto* view = qobject_cast<QQuickWidget*>(watched)) {
                // 장면 밖 좌표로 hover를 한 번 보내면 Quick이 "커서 항목 없음"으로 보고
                // 창에 걸어 둔 커서 모양까지 스스로 되돌립니다. 상태도 함께 풀려서
                // 같은 버튼으로 되돌아왔을 때 손 모양이 다시 걸립니다.
                const QPointF outside(-1, -1);
                QHoverEvent hover(QEvent::HoverLeave, outside, outside, outside);
                QCoreApplication::sendEvent(view->quickWindow(), &hover);
            }
        }

        return QObject::eventFilter(watched, event);
    }
};

/**
 * @brief      QQuickWidget에 커서 복원 필터를 붙입니다.
 * @param view 필터를 붙일 QQuickWidget
 */
inline void installQuickCursorReset(QQuickWidget* view) {
    static auto* filter = new QuickCursorResetFilter(QCoreApplication::instance());
    view->installEventFilter(filter);
}

/**
 * @brief             패널 안에 끼워 넣을 투명 QQuickWidget을 만들고 루트 속성을 설정합니다.
 * @param qmlFile     qrc:/qml/ 아래의 파일 이름
 * @param parent      QQuickWidget을 담을 부모 위젯
 * @param properties  루트 객체에 설정할 속성 모음
 * @return            로딩에 성공한 QQuickWidget, 실패 시 nullptr
 *
 * @details 배경 투명은 WA_TranslucentBackground와 WA_AlwaysStackOnTop이 함께 있어야 동작합니다.
 */
inline QQuickWidget* createQmlPanelView(const QString& qmlFile, QWidget* parent, const QVariantMap& properties = {}) {
    auto* view = new QQuickWidget(sharedQmlEngine(), parent);
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view->setClearColor(Qt::transparent);
    view->setAttribute(Qt::WA_TranslucentBackground);
    view->setAttribute(Qt::WA_AlwaysStackOnTop);
    view->setFocusPolicy(Qt::NoFocus);
    installQuickCursorReset(view);
    view->setSource(QUrl(QStringLiteral("qrc:/qml/") + qmlFile));

    if (!view->rootObject()) {
        qWarning() << "[Qml] failed to load" << qmlFile << view->errors();
        // setSource()가 이 위젯 앞으로 이벤트를 걸어 둔 상태일 수 있어 바로 delete하지 않는다.
        // 부모에서 먼저 떼어 두면 로딩에 실패한 빈 위젯이 그 사이에 화면에 끼지 않는다
        view->setParent(nullptr);
        view->deleteLater();
        return nullptr;
    }

    for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
        view->rootObject()->setProperty(property.key().toUtf8().constData(), property.value());
    }

    return view;
}
