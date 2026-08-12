#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QVariant>
#include <cstdio>

#include "ui/TableModelRoles.h"

namespace {
int failureCount = 0;

/**
 * @brief              조건이 거짓이면 실패로 기록합니다.
 * @param condition    확인할 조건
 * @param description  실패 시 출력할 설명
 */
void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

/**
 * @brief         두 이름표가 같은 실체를 공유하는지 판별할 기준 주소를 돌려줍니다.
 * @param names   확인할 역할 이름표
 * @return        첫 값의 주소, 비어 있으면 nullptr
 */
const void* firstValueAddress(const QHash<int, QByteArray>& names) {
    return names.isEmpty() ? nullptr : static_cast<const void*>(&names.constBegin().value());
}
}  // namespace

int main() {
    const QHash<int, QByteArray> first = tableModelRoleNames();
    const QHash<int, QByteArray> second = tableModelRoleNames();

    check(!first.isEmpty(), "역할 이름표가 비어 있습니다");
    check(first.value(Qt::DisplayRole) == QByteArray("display"), "display 역할이 없습니다");
    check(first.value(TextColorRole) == QByteArray("textColor"), "textColor 역할이 없습니다");
    check(first.value(RowColorRole) == QByteArray("rowColor"), "rowColor 역할이 없습니다");
    check(first.value(IconSourceRole) == QByteArray("iconSource"), "iconSource 역할이 없습니다");

    // QQmlAdaptorModel은 roleNames()를 여러 번 부른 뒤 서로 다른 호출 결과의 iterator를 짝지어 씁니다.
    // 호출마다 새 QHash를 만들면 delegate 생성 중 heap이 깨지므로(RtlFreeHeap 즉사),
    // 매 호출이 같은 실체를 공유하는지 반드시 지켜야 합니다.
    check(firstValueAddress(first) == firstValueAddress(second),
          "roleNames()가 호출마다 새 QHash를 만듭니다 (QML 표에서 heap이 깨집니다)");

    // 사라진 행을 view가 한 번 더 물어볼 때 invalid를 주면 QML이 color를 못 만들고
    // 문자열 셀에 "undefined"가 보입니다. QML이 읽는 역할은 모두 유효한 빈 값이어야 합니다.
    for (auto role = first.constBegin(); role != first.constEnd(); ++role) {
        const QVariant emptyValue = emptyCellValueForRole(role.key());
        check(emptyValue.isValid(), "빈 셀 값이 invalid입니다 (QML 로그가 쏟아집니다)");
        check(role.key() == TextColorRole || role.key() == RowColorRole ? emptyValue.canConvert<QColor>()
                                                                        : emptyValue.canConvert<QString>(),
              "빈 셀 값의 형이 역할과 맞지 않습니다");
    }

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("all table model role checks passed\n");
    return 0;
}
