#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QString>
#include <QVariant>
#include <Qt>

/**
 * @brief  QML 표(DataTable.qml)가 읽는 추가 역할입니다.
 *
 * @details 위젯 표는 Qt::ForegroundRole/BackgroundRole/DecorationRole을 쓰지만 QML에서는
 * QBrush/QIcon을 그대로 쓸 수 없어, 같은 값을 QColor와 url로 노출하는 역할을 따로 둡니다.
 * 두 model이 같은 delegate를 쓰므로 역할 번호와 이름은 이 파일에서만 정의합니다.
 */
enum TableModelRole {
    TextColorRole = Qt::UserRole + 1,
    RowColorRole,
    IconSourceRole,
};

/**
 * @brief   QML delegate가 쓰는 역할 이름표를 반환합니다.
 * @return  매번 같은 실체를 공유하는 역할 번호 → QML 속성 이름 대응표
 *
 * @details roleNames()를 override하는 model은 **반드시** 이 함수처럼 한 번 만든 값을 돌려줘야
 * 합니다. QQmlAdaptorModel이 roleNames()를 여러 번 호출한 뒤 서로 다른 호출 결과의 iterator를
 * 짝지어 쓰기 때문에, 호출마다 새 QHash를 만들면 delegate 생성 중 heap이 깨집니다
 * (`QQmlTableInstanceModel::resolveModelItem` 안 RtlFreeHeap에서 즉사).
 * Qt 기본 구현이 멀쩡한 이유도 정적 hash 하나를 공유해 돌려주기 때문입니다.
 */
inline QHash<int, QByteArray> tableModelRoleNames() {
    static const QHash<int, QByteArray> names = {{Qt::DisplayRole, QByteArray("display")},
                                                 {TextColorRole, QByteArray("textColor")},
                                                 {RowColorRole, QByteArray("rowColor")},
                                                 {IconSourceRole, QByteArray("iconSource")}};

    return names;
}

/**
 * @brief        범위를 벗어난 index에 돌려줄 빈 셀 값을 반환합니다.
 * @param role   요청된 역할
 * @return       역할에 맞는 빈 값, QML이 모르는 역할이면 invalid
 *
 * @details 행이 지워지는 순간 TableView가 사라진 행을 한 번 더 물어봅니다. 여기서 invalid를
 * 돌려주면 QML이 `color`를 못 만들어 로그가 쏟아지고(`QQuickColorValueType ... QVariant(Invalid)`),
 * 문자열 셀에는 "undefined"가 한 프레임 보입니다. 그래서 역할별로 빈 값을 명시해 돌려줍니다.
 */
inline QVariant emptyCellValueForRole(int role) {
    switch (role) {
        case Qt::DisplayRole:
        case IconSourceRole:
            return QString();
        case TextColorRole:
        case RowColorRole:
            return QColor(Qt::transparent);
        default:
            return {};
    }
}
