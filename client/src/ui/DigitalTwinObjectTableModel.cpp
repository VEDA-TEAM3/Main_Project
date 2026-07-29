#include "ui/DigitalTwinObjectTableModel.h"

#include <QBrush>
#include <QIcon>
#include <QString>
#include <algorithm>
#include <utility>

namespace {
/**
 * @brief             객체 종류를 화면 표시용 한글 이름으로 변환합니다.
 * @param objectType  디지털 트윈 객체 종류
 * @return            객체 종류 표시 문자열
 */
QString objectTypeText(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral("차량");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral("보행자");
    }

    return QStringLiteral("-");
}

/**
 * @brief             객체 종류별 기본 아이콘 resource 경로를 반환합니다.
 * @param objectType  디지털 트윈 객체 종류
 * @return            Qt resource icon path
 */
QString objectTypeIconPath(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral(":/icons/vehicle_icon.png");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral(":/icons/human_icon.png");
    }

    return QStringLiteral(":/icons/vehicle_icon.png");
}

/**
 * @brief         정규화 좌표를 목록 표시용 좌표 문자열로 변환합니다.
 * @param object  좌표를 표시할 객체
 * @return        0~100 기준 좌표 문자열
 */
QString positionTextForObject(const DigitalTwinObject& object) {
    return QStringLiteral("(%1, %2)")
        .arg(object.position.x() * 100.0, 0, 'f', 1)
        .arg(object.position.y() * 100.0, 0, 'f', 1);
}

/**
 * @brief         구역 판정 구현 전까지 사용할 안정적인 더미 구역명을 반환합니다.
 * @param object  더미 구역을 배정할 객체
 * @return        CH-01~CH-04 중 하나
 */
QString dummyAreaForObject(const DigitalTwinObject& object) {
    return QStringLiteral("CH-%1").arg(object.channelIndex + 1, 2, 10, QLatin1Char('0'));
}

/**
 * @brief         위험 단계에 맞는 행 글자색을 반환합니다.
 * @param object  표시할 객체
 * @return        목록에 사용할 foreground brush
 */
QBrush foregroundBrushForObject(const DigitalTwinObject& object) {
    if (object.riskLevel == DigitalTwinRiskLevel::Danger) {
        return QBrush(QColor(QStringLiteral("#ff5a5f")));
    }

    if (object.riskLevel == DigitalTwinRiskLevel::Warning) {
        return QBrush(QColor(QStringLiteral("#ffd43b")));
    }

    return QBrush(QColor(QStringLiteral("#d8e3f2")));
}

/**
 * @brief          객체 목록을 ID 기준으로 정렬합니다.
 * @param objects  정렬할 객체 목록
 * @return         화면 표시 순서가 안정적인 객체 목록
 */
QVector<DigitalTwinObject> sortedObjectsById(QVector<DigitalTwinObject> objects) {
    std::sort(objects.begin(), objects.end(),
              [](const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject) {
                  return firstObject.objectId < secondObject.objectId;
              });

    return objects;
}
}  // namespace

/**
 * @brief         실시간 객체 목록 표시용 table model을 생성합니다.
 * @param parent  Qt 객체 소유권 부모
 */
DigitalTwinObjectTableModel::DigitalTwinObjectTableModel(QObject* parent) : QAbstractTableModel(parent) {}

/**
 * @brief         현재 표시할 객체 행 개수를 반환합니다.
 * @param parent  table model에서는 사용하지 않는 부모 index
 * @return        객체 개수
 */
int DigitalTwinObjectTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }

    return static_cast<int>(objects_.size());
}

/**
 * @brief         실시간 객체 목록의 열 개수를 반환합니다.
 * @param parent  table model에서는 사용하지 않는 부모 index
 * @return        ID, 유형, 위치, 구역 열 개수
 */
int DigitalTwinObjectTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }

    return ObjectListColumnCount;
}

/**
 * @brief        view가 요청한 셀 데이터를 반환합니다.
 * @param index  요청된 model index
 * @param role   표시 역할
 * @return       역할에 맞는 표시 데이터
 */
QVariant DigitalTwinObjectTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= objects_.size()) {
        return {};
    }

    const DigitalTwinObject& object = objects_[index.row()];

    if (role == Qt::TextAlignmentRole) {
        return Qt::AlignCenter;
    }

    if (role == Qt::ForegroundRole) {
        return foregroundBrushForObject(object);
    }

    if (role == Qt::DecorationRole && index.column() == ObjectTypeColumn) {
        return QIcon(objectTypeIconPath(object.type));
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
        case ObjectIdColumn:
            return object.objectId;
        case ObjectTypeColumn:
            return objectTypeText(object.type);
        case ObjectPositionColumn:
            return positionTextForObject(object);
        case ObjectAreaColumn:
            return dummyAreaForObject(object);
        default:
            return {};
    }
}

/**
 * @brief              표 헤더 표시 문자열을 반환합니다.
 * @param section      헤더 열/행 번호
 * @param orientation  수평/수직 헤더 방향
 * @param role         표시 역할
 * @return             헤더 데이터
 */
QVariant DigitalTwinObjectTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
        case ObjectIdColumn:
            return QStringLiteral("ID");
        case ObjectTypeColumn:
            return QStringLiteral("유형");
        case ObjectPositionColumn:
            return QStringLiteral("위치 (X, Y)");
        case ObjectAreaColumn:
            return QStringLiteral("채널");
        default:
            return {};
    }
}

/**
 * @brief        view item의 동작 플래그를 반환합니다.
 * @param index  요청된 model index
 * @return       선택/편집 없이 표시만 허용하는 플래그
 */
Qt::ItemFlags DigitalTwinObjectTableModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled;
}

/**
 * @brief          worker에서 전달된 최신 객체 목록을 model에 반영합니다.
 * @param objects  최신 객체 목록
 */
void DigitalTwinObjectTableModel::updateObjects(QVector<DigitalTwinObject> objects) {
    QVector<DigitalTwinObject> sortedObjects = sortedObjectsById(std::move(objects));

    if (!hasSameIdentityOrder(sortedObjects)) {
        beginResetModel();
        objects_ = std::move(sortedObjects);
        endResetModel();
        return;
    }

    objects_ = std::move(sortedObjects);

    if (!objects_.isEmpty()) {
        emit dataChanged(index(0, 0), index(static_cast<int>(objects_.size() - 1), ObjectListColumnCount - 1),
                         {Qt::DisplayRole, Qt::DecorationRole, Qt::ForegroundRole});
    }
}

/**
 * @brief          기존 행 순서와 새 객체 ID 순서가 같은지 확인합니다.
 * @param objects  비교할 새 객체 목록
 * @return         행 구조 변경 없이 dataChanged만 emit해도 되면 true
 */
bool DigitalTwinObjectTableModel::hasSameIdentityOrder(const QVector<DigitalTwinObject>& objects) const {
    if (objects_.size() != objects.size()) {
        return false;
    }

    for (int index = 0; index < objects.size(); ++index) {
        if (objects_[index].objectId != objects[index].objectId) {
            return false;
        }
    }

    return true;
}
