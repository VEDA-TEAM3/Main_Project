#include "network/ReportGateway.h"

/**
 * @brief        신고 전송 구현체가 공통으로 사용하는 기반 객체를 생성합니다.
 * @param parent Qt 객체 소유권을 연결할 부모 객체
 */
ReportGateway::ReportGateway(QObject* parent) : QObject(parent) {}

/**
 * @brief 신고 전송 기반 객체를 해제합니다.
 */
ReportGateway::~ReportGateway() = default;
