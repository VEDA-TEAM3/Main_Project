#pragma once

#include <QHash>
#include <QString>

#include "auth/AuthGateway.h"

/**
 * @brief 실행 파일 옆 users.json으로 관제 계정을 확인하는 인증 구현.
 *
 * @details 프로그램을 폴더째 옮겨도 동작해야 하므로 머신에 묶이는 저장소(DPAPI, 레지스트리,
 *          %PROGRAMDATA%)를 쓰지 않습니다. 그래서 폴더를 복사해 간 사람은 막지 못합니다 -
 *          이 게이트의 값어치는 콘솔 앞 무단 조작 방지, 권한 분리, 행위 귀속입니다.
 *
 *          비밀번호를 PBKDF2로 저장하는 이유도 앱이 아니라 사용자를 지키기 위해서입니다.
 *          사람들은 비밀번호를 재사용하므로, 파일이 유출됐을 때 평문이나 단순 해시가
 *          들어 있으면 그 사람의 다른 계정까지 넘어갑니다.
 */
class LocalFileAuthGateway final : public AuthGateway {
public:
    explicit LocalFileAuthGateway(QString usersFilePath);

    AuthResult authenticate(const QString& userName, const QString& password) override;
    bool needsBootstrap() const override;
    bool createInitialAdmin(const QString& userName, const QString& password, QString& error) override;

    /**
     * @brief           계정을 만들어 파일에 저장합니다.
     * @param userName  아이디
     * @param password  비밀번호
     * @param role      "admin" 또는 "operator"
     * @param error     실패 사유
     * @return          저장에 성공하면 true
     */
    bool createUser(const QString& userName, const QString& password, const QString& role, QString& error);

    /// 설정 파일 경로에서 계정 파일 경로를 만듭니다 (같은 폴더의 users.json)
    static QString usersFilePathFor(const QString& configFilePath);

private:
    /// 연속 실패에 대한 지연 상태
    struct FailureState {
        int count = 0;
        qint64 blockedUntilMsec = 0;
    };

    QString usersFilePath_;
    /// ponytail: 실패 횟수는 프로세스 메모리에만 둔다. 재시작하면 초기화되므로 콘솔 앞
    /// 공격자에게는 상한이 없다. 파일로 옮기려면 그 파일도 같이 옮겨 다녀야 한다
    QHash<QString, FailureState> failures_;
};
