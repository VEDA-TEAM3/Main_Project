#include "ui/EventLogTableModel.h"

#include <QBrush>
#include <QColor>
#include <QString>
#include <utility>

namespace {
constexpr int maximumEventLogRows = 120;

/**
 * @brief           이벤트 위험 단계를 화면 표시용 한글 문자열로 변환합니다.
 * @param riskLevel  이벤트 위험 단계
 * @return          위험 단계 표시 문자열
 */
QString riskLevelText(EventLogRiskLevel riskLevel) {
    switch (riskLevel) {
        case EventLogRiskLevel::Normal:
            return QStringLiteral("정상");
        case EventLogRiskLevel::Warning:
            return QStringLiteral("주의");
        case EventLogRiskLevel::Danger:
            return QStringLiteral("위험");
    }

    return QStringLiteral("-");
}

/**
 * @brief         이벤트 조치 사항을 화면 표시용 한글 문자열로 변환합니다.
 * @param action  이벤트 조치 사항
 * @return        조치 사항 표시 문자열
 */
QString actionText(EventLogAction action) {
    switch (action) {
        case EventLogAction::None:
            return QStringLiteral("-");
        case EventLogAction::WarningAlertActivated:
            return QStringLiteral("경고 알림 가동");
        case EventLogAction::DangerAlertActivated:
            return QStringLiteral("위험 알림 가동");
    }

    return QStringLiteral("-");
}

/**
 * @brief           이벤트 위험 단계별 표시 색상을 반환합니다.
 * @param riskLevel  이벤트 위험 단계
 * @return          table view에 적용할 foreground brush
 */
QBrush foregroundBrushForRiskLevel(EventLogRiskLevel riskLevel) {
    switch (riskLevel) {
        case EventLogRiskLevel::Danger:
            return QBrush(QColor(QStringLiteral("#ff5a5f")));
        case EventLogRiskLevel::Warning:
            return QBrush(QColor(QStringLiteral("#ffd43b")));
        case EventLogRiskLevel::Normal:
            return QBrush(QColor(QStringLiteral("#35d04f")));
    }

    return QBrush(QColor(QStringLiteral("#d8e3f2")));
}

/**
 * @brief           이벤트 위험 단계별 행 배경색을 반환합니다.
 * @param riskLevel  이벤트 위험 단계
 * @return           위험/주의 행 강조용 background brush
 */
QBrush backgroundBrushForRiskLevel(EventLogRiskLevel riskLevel) {
    switch (riskLevel) {
        case EventLogRiskLevel::Danger:
            return QBrush(QColor(QStringLiteral("#2a1119")));
        case EventLogRiskLevel::Warning:
            return QBrush(QColor(QStringLiteral("#2a2410")));
        case EventLogRiskLevel::Normal:
            return {};
    }

    return {};
}
}  // namespace

/**
 * @brief         이벤트 로그 표시용 table model을 생성합니다.
 * @param parent  Qt 객체 소유권 부모
 */
EventLogTableModel::EventLogTableModel(QObject* parent) : QAbstractTableModel(parent) {}

/**
 * @brief         현재 보관 중인 이벤트 로그 행 개수를 반환합니다.
 * @param parent  table model에서는 사용하지 않는 부모 index
 * @return        이벤트 로그 행 개수
 */
int EventLogTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }

    return static_cast<int>(entries_.size());
}

/**
 * @brief         이벤트 로그 table의 열 개수를 반환합니다.
 * @param parent  table model에서는 사용하지 않는 부모 index
 * @return        시간, 구역, 객체, 위험 수준, 조치 사항 열 개수
 */
int EventLogTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }

    return EventLogColumnCount;
}

/**
 * @brief        view가 요청한 이벤트 로그 셀 데이터를 반환합니다.
 * @param index  요청된 model index
 * @param role   표시 역할
 * @return       역할에 맞는 표시 데이터
 */
QVariant EventLogTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
        return {};
    }

    const EventLogEntry& entry = entries_[index.row()];

    if (role == Qt::TextAlignmentRole) {
        return Qt::AlignCenter;
    }

    if (role == Qt::BackgroundRole) {
        return backgroundBrushForRiskLevel(entry.riskLevel);
    }

    if (role == Qt::ForegroundRole) {
        if (entry.riskLevel == EventLogRiskLevel::Normal && index.column() != EventRiskColumn) {
            return QBrush(QColor(QStringLiteral("#d8e3f2")));
        }

        return foregroundBrushForRiskLevel(entry.riskLevel);
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
        case EventTimeColumn:
            return entry.time.toString(QStringLiteral("HH:mm:ss"));
        case EventAreaColumn:
            return entry.area;
        case EventObjectColumn:
            return entry.objectText;
        case EventRiskColumn:
            return riskLevelText(entry.riskLevel);
        case EventActionColumn:
            return actionText(entry.action);
        default:
            return {};
    }
}

/**
 * @brief              이벤트 로그 table 헤더 문자열을 반환합니다.
 * @param section      헤더 열/행 번호
 * @param orientation  수평/수직 헤더 방향
 * @param role         표시 역할
 * @return             헤더 데이터
 */
QVariant EventLogTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
        case EventTimeColumn:
            return QStringLiteral("시간");
        case EventAreaColumn:
            return QStringLiteral("채널");
        case EventObjectColumn:
            return QStringLiteral("이벤트");
        case EventRiskColumn:
            return QStringLiteral("위험 수준");
        case EventActionColumn:
            return QStringLiteral("조치 사항");
        default:
            return {};
    }
}

/**
 * @brief        이벤트 로그 view item의 동작 플래그를 반환합니다.
 * @param index  요청된 model index
 * @return       선택/편집 없이 표시만 허용하는 플래그
 */
Qt::ItemFlags EventLogTableModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled;
}

/**
 * @brief        새 이벤트 로그를 최상단에 추가하고 오래된 로그를 제한 개수 이상 보관하지 않습니다.
 * @param entry  추가할 이벤트 로그
 */
void EventLogTableModel::prependEntry(const EventLogEntry& entry) { prependEntries({entry}); }

/**
 * @brief          여러 이벤트를 한 번의 model 변경으로 최상단에 추가합니다.
 * @param entries  추가할 이벤트 로그 목록
 */
void EventLogTableModel::prependEntries(QVector<EventLogEntry> entries) {
    if (entries.isEmpty()) {
        return;
    }

    const int insertedRowCount = static_cast<int>(entries.size());
    beginInsertRows(QModelIndex(), 0, insertedRowCount - 1);
    entries_ = std::move(entries) + entries_;
    endInsertRows();

    if (entries_.size() <= maximumEventLogRows) {
        return;
    }

    beginRemoveRows(QModelIndex(), maximumEventLogRows, static_cast<int>(entries_.size() - 1));
    entries_.erase(entries_.begin() + maximumEventLogRows, entries_.end());
    endRemoveRows();
}

/** @brief 현재 표시 중인 모든 이벤트 로그를 모델에서 제거합니다. */
void EventLogTableModel::clear() {
    if (entries_.isEmpty()) {
        return;
    }

    beginResetModel();
    entries_.clear();
    endResetModel();
}
