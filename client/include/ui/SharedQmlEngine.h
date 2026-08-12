#pragma once

#include <QCoreApplication>
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
    view->setSource(QUrl(QStringLiteral("qrc:/qml/") + qmlFile));

    if (!view->rootObject()) {
        qWarning() << "[Qml] failed to load" << qmlFile << view->errors();
        delete view;
        return nullptr;
    }

    for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
        view->rootObject()->setProperty(property.key().toUtf8().constData(), property.value());
    }

    return view;
}
