#include "ui/DigitalTwinObjectTableModel.h"

#include <QColor>
#include <QString>
#include <algorithm>
#include <utility>

#include "ui/TableModelRoles.h"

namespace {
constexpr int channelsPerArea = 4;
constexpr int minimumZoneId = 0;

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
 * @brief             객체 종류별 기본 아이콘 resource URL을 반환합니다.
 * @param objectType  디지털 트윈 객체 종류
 * @return            QML Image가 읽을 수 있는 qrc URL
 */
QString objectTypeIconUrl(DigitalTwinObjectType objectType) {
    switch (objectType) {
        case DigitalTwinObjectType::Vehicle:
            return QStringLiteral("qrc:/icons/vehicle_icon.png");
        case DigitalTwinObjectType::Pedestrian:
            return QStringLiteral("qrc:/icons/human_icon.png");
    }

    return QStringLiteral("qrc:/icons/vehicle_icon.png");
}

/**
 * @brief         정규화 좌표를 목록 표시용 좌표 문자열로 변환합니다.
 * @param object  좌표를 표시할 객체
 * @return        0~100 기준 좌표 문자열
 */
QString positionTextForObject(const DigitalTwinObject& object) {
    return QStringLiteral("(%1, %2)").arg(object.position.x(), 0, 'f', 1).arg(object.position.y(), 0, 'f', 1);
}

/**
 * @brief         서버 zoneId 기준의 물리 CCTV 구역명을 반환합니다.
 * @param zoneId  서버가 확정한 0 기반 zoneId
 * @return        1구역, 2구역 또는 미배정 표시
 */
QString areaTextForZoneId(int zoneId) {
    if (zoneId < minimumZoneId) {
        return QStringLiteral("미배정");
    }

    return QStringLiteral("%1구역").arg(zoneId / channelsPerArea + 1);
}

/**
 * @brief         서버 zoneId를 해당 구역의 1~4 채널 표시로 변환합니다.
 * @param zoneId  서버가 확정한 0 기반 zoneId
 * @return        CH-01~CH-04 또는 미배정 표시
 */
QString localChannelTextForZoneId(int zoneId) {
    if (zoneId < minimumZoneId) {
        return QStringLiteral("-");
    }

    return QStringLiteral("CH-%1").arg(zoneId % channelsPerArea + 1, 2, 10, QLatin1Char('0'));
}

/**
 * @brief         위험 단계에 맞는 행 글자색을 반환합니다.
 * @param object  표시할 객체
 * @return        목록에 사용할 글자색
 */
QColor textColorForObject(const DigitalTwinObject& object) {
    if (object.riskLevel == DigitalTwinRiskLevel::Danger) {
        return QColor(QStringLiteral("#ff5a5f"));
    }

    if (object.riskLevel == DigitalTwinRiskLevel::Warning) {
        return QColor(QStringLiteral("#ffd43b"));
    }

    return QColor(QStringLiteral("#d8e3f2"));
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
        return emptyCellValueForRole(role);
    }

    const DigitalTwinObject& object = objects_[index.row()];

    if (role == TextColorRole) {
        return textColorForObject(object);
    }

    // 객체 목록은 행 배경을 강조하지 않고 표가 알아서 줄무늬를 넣게 둡니다.
    if (role == RowColorRole) {
        return QColor(Qt::transparent);
    }

    if (role == IconSourceRole) {
        return index.column() == ObjectTypeColumn ? objectTypeIconUrl(object.type) : QString();
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
        case ObjectZoneColumn:
            return areaTextForZoneId(object.channelIndex);
        case ObjectChannelColumn:
            return localChannelTextForZoneId(object.channelIndex);
        default:
            return QString();
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
        case ObjectZoneColumn:
            return QStringLiteral("구역");
        case ObjectChannelColumn:
            return QStringLiteral("채널");
        default:
            return {};
    }
}

/**
 * @brief   QML delegate가 읽을 역할 이름표를 반환합니다.
 * @return  역할 번호 → QML 속성 이름 대응표
 */
QHash<int, QByteArray> DigitalTwinObjectTableModel::roleNames() const { return tableModelRoleNames(); }

/**
 * @brief          worker에서 전달된 최신 객체 목록을 model에 반영합니다.
 * @param objects  최신 객체 목록
 */
void DigitalTwinObjectTableModel::updateObjects(QVector<DigitalTwinObject> objects) {
    const QVector<DigitalTwinObject> sortedObjects = sortedObjectsById(std::move(objects));

    // 초당 다섯 번 model 전체를 reset하면 view가 표시 상태를 매번 버립니다.
    // 양쪽 다 objectId로 정렬돼 있으므로 사라진 행과 새로 생긴 행만 알립니다.
    int row = 0;
    while (row < objects_.size()) {
        const QString existingId = objects_[row].objectId;
        const auto stillAlive = std::find_if(
            sortedObjects.cbegin(), sortedObjects.cend(),
            [&existingId](const DigitalTwinObject& candidate) { return candidate.objectId == existingId; });
        if (stillAlive == sortedObjects.cend()) {
            beginRemoveRows(QModelIndex(), row, row);
            objects_.remove(row);
            endRemoveRows();
            continue;
        }
        ++row;
    }

    for (int targetRow = 0; targetRow < sortedObjects.size(); ++targetRow) {
        if (targetRow >= objects_.size() || objects_[targetRow].objectId != sortedObjects[targetRow].objectId) {
            beginInsertRows(QModelIndex(), targetRow, targetRow);
            objects_.insert(targetRow, sortedObjects[targetRow]);
            endInsertRows();
            continue;
        }

        objects_[targetRow] = sortedObjects[targetRow];
    }

    // 역할을 나열하면 빠뜨린 역할이 갱신되지 않아 아이콘과 글자가 어긋납니다. 전체 역할을 알립니다.
    if (!objects_.isEmpty()) {
        emit dataChanged(index(0, 0), index(static_cast<int>(objects_.size() - 1), ObjectListColumnCount - 1));
    }
}
