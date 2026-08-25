#pragma once

#include <QString>

/// 로그인한 관제 사용자. role은 "admin" 또는 "operator"다
struct AuthenticatedUser {
    QString name;
    QString role;

    bool isAdmin() const { return role == QStringLiteral("admin"); }
    bool isValid() const { return !name.isEmpty(); }
};

struct AuthResult {
    bool successful = false;
    AuthenticatedUser user;
    /// 화면에 그대로 보여줄 실패 사유
    QString error;
};

/**
 * @brief 관제 계정 인증 경계.
 *
 * @details 지금 구현은 실행 파일 옆 users.json을 읽는 LocalFileAuthGateway 하나다.
 *          관제 PC가 여러 대가 되면 control-server를 보는 구현으로 갈아 끼운다.
 */
class AuthGateway {
public:
    virtual ~AuthGateway() = default;

    /**
     * @brief           아이디와 비밀번호를 확인합니다.
     * @param userName  입력된 아이디
     * @param password  입력된 비밀번호
     * @return          성공 여부와 사용자 정보, 실패 시 표시할 사유
     */
    virtual AuthResult authenticate(const QString& userName, const QString& password) = 0;

    /// 계정이 하나도 없어 최초 관리자 생성이 필요한 상태인지
    virtual bool needsBootstrap() const = 0;

    /**
     * @brief           계정이 하나도 없을 때 첫 관리자를 만듭니다.
     * @param userName  아이디
     * @param password  비밀번호
     * @param error     실패 사유
     * @return          생성에 성공하면 true
     *
     * @details 이미 계정이 있으면 실패해야 합니다. 이 경로로 관리자를 덧붙일 수 있으면
     *          최초 실행 화면이 권한 상승 통로가 됩니다.
     */
    virtual bool createInitialAdmin(const QString& userName, const QString& password, QString& error) = 0;
};
