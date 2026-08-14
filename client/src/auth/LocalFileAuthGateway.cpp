#include "auth/LocalFileAuthGateway.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QSaveFile>
#include <utility>

namespace {
constexpr int usersFileVersion = 1;
constexpr int saltByteCount = 16;
constexpr int derivedKeyByteCount = 32;
/// 새 계정에 쓰는 반복 횟수. 레코드마다 저장하므로 나중에 올려도 기존 계정이 그대로 열린다
constexpr int defaultIterations = 210000;
/// 터무니없는 값이 파일에 들어와도 로그인 한 번이 몇 분씩 걸리지 않게 막는다
constexpr int maximumIterations = 2000000;
constexpr int failuresBeforeDelay = 3;
constexpr int maximumDelaySeconds = 30;

/**
 * @brief   길이와 내용을 상수 시간으로 비교합니다.
 * @details 이른 반환으로 앞 몇 바이트가 맞았는지 흘리지 않기 위한 것입니다.
 */
bool equalsInConstantTime(const QByteArray& first, const QByteArray& second) {
    if (first.size() != second.size()) {
        return false;
    }

    quint8 difference = 0;
    for (qsizetype index = 0; index < first.size(); ++index) {
        difference |= static_cast<quint8>(first.at(index)) ^ static_cast<quint8>(second.at(index));
    }
    return difference == 0;
}

QByteArray derive(const QString& password, const QByteArray& salt, int iterations) {
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(), salt, iterations,
                                              derivedKeyByteCount);
}

QByteArray randomSalt() {
    QByteArray salt(saltByteCount, Qt::Uninitialized);
    QRandomGenerator::system()->generate(salt.begin(), salt.end());
    return salt;
}

bool readUsersDocument(const QString& path, QJsonObject& root, QString& error) {
    QFile file(path);
    if (!file.exists()) {
        root = QJsonObject{{QStringLiteral("version"), usersFileVersion}, {QStringLiteral("users"), QJsonArray{}}};
        return true;
    }

    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        error = QStringLiteral("계정 파일을 열 수 없습니다: %1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("계정 파일 형식이 올바르지 않습니다: %1").arg(parseError.errorString());
        return false;
    }

    root = document.object();
    return true;
}

bool writeUsersDocument(const QString& path, const QJsonObject& root, QString& error) {
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        error = QStringLiteral("계정 파일에 쓸 수 없습니다: %1").arg(file.errorString());
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        error = QStringLiteral("계정 파일 저장에 실패했습니다: %1").arg(file.errorString());
        return false;
    }
    return true;
}

/** @brief 아이디로 사용자 레코드를 찾습니다. 대소문자는 구분하지 않습니다. */
QJsonObject findUser(const QJsonArray& users, const QString& userName) {
    for (const QJsonValue& value : users) {
        const QJsonObject user = value.toObject();
        if (user.value(QStringLiteral("name")).toString().compare(userName, Qt::CaseInsensitive) == 0) {
            return user;
        }
    }
    return {};
}
}  // namespace

/**
 * @brief               계정 파일 경로를 받아 인증기를 만듭니다.
 * @param usersFilePath users.json 절대 경로
 */
LocalFileAuthGateway::LocalFileAuthGateway(QString usersFilePath) : usersFilePath_(std::move(usersFilePath)) {}

/**
 * @brief                  설정 파일과 같은 폴더의 계정 파일 경로를 만듭니다.
 * @param configFilePath   ApplicationConfigLoadResult::sourcePath
 *
 * @details 설정 파일 해석기가 이미 실행 파일 옆을 먼저 보므로, 그 결과를 따라가면
 *          VEDA_CONFIG_FILE로 설정을 다른 곳에 둔 경우까지 자동으로 맞습니다.
 */
QString LocalFileAuthGateway::usersFilePathFor(const QString& configFilePath) {
    return QFileInfo(configFilePath).dir().filePath(QStringLiteral("users.json"));
}

/** @brief 계정이 하나도 없으면 최초 관리자 생성이 필요합니다. */
bool LocalFileAuthGateway::needsBootstrap() const {
    QJsonObject root;
    QString error;
    if (!readUsersDocument(usersFilePath_, root, error)) {
        return false;
    }
    return root.value(QStringLiteral("users")).toArray().isEmpty();
}

/**
 * @brief           아이디와 비밀번호를 확인합니다.
 * @param userName  입력된 아이디
 * @param password  입력된 비밀번호
 * @return          성공 여부와 사용자 정보, 실패 시 표시할 사유
 */
AuthResult LocalFileAuthGateway::authenticate(const QString& userName, const QString& password) {
    AuthResult result;
    const QString key = userName.trimmed().toLower();
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();

    FailureState& failure = failures_[key];
    if (failure.blockedUntilMsec > nowMsec) {
        const qint64 secondsLeft = (failure.blockedUntilMsec - nowMsec + 999) / 1000;
        result.error = QStringLiteral("시도가 너무 잦습니다. %1초 후에 다시 시도하세요.").arg(secondsLeft);
        return result;
    }

    QJsonObject root;
    QString error;
    if (!readUsersDocument(usersFilePath_, root, error)) {
        result.error = error;
        return result;
    }

    const QJsonObject user = findUser(root.value(QStringLiteral("users")).toArray(), userName.trimmed());
    const QByteArray salt = QByteArray::fromBase64(user.value(QStringLiteral("salt")).toString().toUtf8());
    const QByteArray storedHash = QByteArray::fromBase64(user.value(QStringLiteral("hash")).toString().toUtf8());
    const int iterations = user.value(QStringLiteral("iterations")).toInt();

    // 아이디가 없어도 같은 실패 경로로 내려보낸다. 존재 여부를 응답 차이로 흘리지 않는다
    const bool usable = !salt.isEmpty() && !storedHash.isEmpty() && iterations > 0 && iterations <= maximumIterations;
    const bool disabled = user.value(QStringLiteral("disabled")).toBool(false);
    if (!usable || disabled || !equalsInConstantTime(derive(password, salt, iterations), storedHash)) {
        ++failure.count;
        if (failure.count >= failuresBeforeDelay) {
            const int delaySeconds = qMin(maximumDelaySeconds, 1 << qMin(5, failure.count - failuresBeforeDelay + 1));
            failure.blockedUntilMsec = nowMsec + delaySeconds * 1000LL;
        }
        result.error = QStringLiteral("아이디 또는 비밀번호가 올바르지 않습니다.");
        return result;
    }

    failures_.remove(key);
    result.successful = true;
    result.user.name = user.value(QStringLiteral("name")).toString();
    result.user.role = user.value(QStringLiteral("role")).toString(QStringLiteral("operator"));
    return result;
}

/**
 * @brief           계정이 하나도 없을 때만 첫 관리자를 만듭니다.
 * @param userName  아이디
 * @param password  비밀번호
 * @param error     실패 사유
 * @return          생성에 성공하면 true
 */
bool LocalFileAuthGateway::createInitialAdmin(const QString& userName, const QString& password, QString& error) {
    // 이미 계정이 있으면 이 경로를 막는다. 열어 두면 최초 실행 화면이 관리자 추가 통로가 된다
    if (!needsBootstrap()) {
        error = QStringLiteral("이미 계정이 있어 최초 관리자를 만들 수 없습니다.");
        return false;
    }
    return createUser(userName, password, QStringLiteral("admin"), error);
}

/**
 * @brief           계정을 만들어 파일에 저장합니다.
 * @param userName  아이디
 * @param password  비밀번호
 * @param role      "admin" 또는 "operator"
 * @param error     실패 사유
 * @return          저장에 성공하면 true
 */
bool LocalFileAuthGateway::createUser(const QString& userName, const QString& password, const QString& role,
                                      QString& error) {
    const QString trimmedName = userName.trimmed();
    if (trimmedName.isEmpty()) {
        error = QStringLiteral("아이디를 입력하세요.");
        return false;
    }
    if (password.isEmpty()) {
        error = QStringLiteral("비밀번호를 입력하세요.");
        return false;
    }
    if (role != QStringLiteral("admin") && role != QStringLiteral("operator")) {
        error = QStringLiteral("역할은 admin 또는 operator여야 합니다.");
        return false;
    }

    QJsonObject root;
    if (!readUsersDocument(usersFilePath_, root, error)) {
        return false;
    }

    QJsonArray users = root.value(QStringLiteral("users")).toArray();
    if (!findUser(users, trimmedName).isEmpty()) {
        error = QStringLiteral("이미 있는 아이디입니다: %1").arg(trimmedName);
        return false;
    }

    const QByteArray salt = randomSalt();
    QJsonObject user;
    user.insert(QStringLiteral("name"), trimmedName);
    user.insert(QStringLiteral("role"), role);
    user.insert(QStringLiteral("salt"), QString::fromUtf8(salt.toBase64()));
    user.insert(QStringLiteral("iterations"), defaultIterations);
    user.insert(QStringLiteral("hash"), QString::fromUtf8(derive(password, salt, defaultIterations).toBase64()));
    user.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    user.insert(QStringLiteral("disabled"), false);
    users.append(user);

    root.insert(QStringLiteral("version"), usersFileVersion);
    root.insert(QStringLiteral("users"), users);
    return writeUsersDocument(usersFilePath_, root, error);
}
