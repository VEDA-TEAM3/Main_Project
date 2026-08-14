#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

#include "auth/LocalFileAuthGateway.h"

namespace {
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

/** @brief 만든 계정이 그대로 다시 열리고, 틀린 비밀번호는 막히는지 검사합니다. */
void checkCreateAndAuthenticate(const QString& usersFilePath) {
    LocalFileAuthGateway gateway(usersFilePath);
    check(gateway.needsBootstrap(), "an empty install must ask for the first admin");

    QString error;
    check(gateway.createUser(QStringLiteral("admin"), QStringLiteral("pw-with-!@:/"), QStringLiteral("admin"), error),
          "creating the first admin must succeed");
    check(!gateway.needsBootstrap(), "bootstrap must be over once an account exists");

    const AuthResult good = gateway.authenticate(QStringLiteral("admin"), QStringLiteral("pw-with-!@:/"));
    check(good.successful, "the stored password must authenticate");
    check(good.user.isAdmin(), "the admin role must survive the round trip");

    const AuthResult wrong = gateway.authenticate(QStringLiteral("admin"), QStringLiteral("pw-with-!@:"));
    check(!wrong.successful, "a wrong password must be rejected");
    check(!wrong.error.isEmpty(), "a rejection must carry a reason to show");

    const AuthResult unknown = gateway.authenticate(QStringLiteral("nobody"), QStringLiteral("pw-with-!@:/"));
    check(!unknown.successful, "an unknown account must be rejected");
    check(unknown.error == wrong.error, "an unknown account must not be told apart from a wrong password");
}

/** @brief 비밀번호가 평문이나 단순 해시로 남지 않는지 검사합니다. */
void checkPasswordIsNotRecoverable(const QString& usersFilePath) {
    QFile file(usersFilePath);
    check(file.open(QFile::ReadOnly), "the users file must exist after creating an account");
    const QByteArray stored = file.readAll();

    check(!stored.contains("pw-with-!@:/"), "the password must never be stored in clear text");
    check(stored.contains("\"iterations\""), "the iteration count must be stored per record so it can be raised later");
    check(stored.contains("\"salt\""), "each account must carry its own salt");
}

/** @brief 같은 아이디를 두 번 만들 수 없고, 역할 값이 검증되는지 검사합니다. */
void checkRejectedInput(const QString& usersFilePath) {
    LocalFileAuthGateway gateway(usersFilePath);
    QString error;

    check(!gateway.createUser(QStringLiteral("admin"), QStringLiteral("other"), QStringLiteral("admin"), error),
          "a duplicate account name must be rejected");
    check(!gateway.createUser(QStringLiteral("guard"), QStringLiteral("pw"), QStringLiteral("superuser"), error),
          "an unknown role must be rejected");
    check(!gateway.createUser(QString(), QStringLiteral("pw"), QStringLiteral("operator"), error),
          "an empty account name must be rejected");
    check(!gateway.createUser(QStringLiteral("guard"), QString(), QStringLiteral("operator"), error),
          "an empty password must be rejected");

    // 최초 실행 화면이 관리자 추가 통로가 되면 안 된다
    check(!gateway.createInitialAdmin(QStringLiteral("second"), QStringLiteral("pw"), error),
          "the bootstrap path must be closed once an account exists");
}

/** @brief 계정 파일이 설정 파일 옆에 놓이는지 검사합니다 (폴더째 옮겨도 따라가야 한다). */
void checkUsersFileFollowsConfig() {
    const QString configPath = QDir::toNativeSeparators(QStringLiteral("/opt/app/config/app_config.json"));
    const QString usersPath = LocalFileAuthGateway::usersFilePathFor(configPath);
    check(QFileInfo(usersPath).fileName() == QStringLiteral("users.json"), "the account file must be users.json");
    check(QFileInfo(usersPath).absolutePath() == QFileInfo(configPath).absolutePath(),
          "the account file must sit beside the configuration it belongs to");
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::fprintf(stderr, "FAIL: could not create a temporary directory\n");
        return 1;
    }

    const QString usersFilePath = QDir(directory.path()).filePath(QStringLiteral("users.json"));
    checkCreateAndAuthenticate(usersFilePath);
    checkPasswordIsNotRecoverable(usersFilePath);
    checkRejectedInput(usersFilePath);
    checkUsersFileFollowsConfig();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("LocalFileAuthGateway checks passed\n");
    return 0;
}
