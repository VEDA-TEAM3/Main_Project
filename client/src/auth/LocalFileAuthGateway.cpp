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
/// 짧은 비밀번호는 PBKDF2를 걸어도 오프라인에서 금방 풀린다. 계정 파일이 프로그램과 함께
/// 옮겨 다니므로 파일이 남의 손에 들어가는 것을 전제로 최소 길이를 강제한다
constexpr int minimumPasswordLength = 8;
/// 파일이 무한정 커지지 않도록 아이디 길이를 묶는다
constexpr int maximumUserNameLength = 64;
/// 실패 기록은 시도된 아이디마다 생기므로, 임의의 아이디를 계속 넣으면 늘어나기만 한다
constexpr int maximumTrackedFailures = 256;

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

    // 같은 PC를 여러 사람이 쓰면 다른 계정이 해시를 그대로 읽어 갈 수 있다. 성공 여부는 보지
    // 않는다 - FAT32 USB처럼 권한이 없는 매체에서는 실패하는 것이 정상이고, 그렇다고 계정
    // 생성을 막을 이유는 없다
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
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

    // 시도된 아이디마다 항목이 생기므로 아무 아이디나 계속 넣으면 늘어나기만 한다.
    // 지연이 끝난 항목은 들고 있을 이유가 없으므로 한도를 넘으면 정리한다
    if (failures_.size() > maximumTrackedFailures) {
        for (auto iterator = failures_.begin(); iterator != failures_.end();) {
            if (iterator.value().blockedUntilMsec <= nowMsec) {
                iterator = failures_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

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

    const bool usable = !salt.isEmpty() && !storedHash.isEmpty() && iterations > 0 && iterations <= maximumIterations;
    const bool disabled = user.value(QStringLiteral("disabled")).toBool(false);

    // 없는 아이디에도 같은 비용을 치른다. 여기서 곧바로 실패로 빠지면 응답이 마이크로초 만에
    // 돌아오고, 실제 계정은 PBKDF2 때문에 수백 ms가 걸린다. 그 차이만으로 어떤 아이디가
    // 존재하는지 훑을 수 있다. 결과는 버리더라도 derive는 반드시 한 번 돈다
    const QByteArray candidateHash =
        derive(password, usable ? salt : QByteArray(saltByteCount, '\0'), usable ? iterations : defaultIterations);
    if (!usable || disabled || !equalsInConstantTime(candidateHash, storedHash)) {
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
    if (trimmedName.size() > maximumUserNameLength) {
        error = QStringLiteral("아이디는 %1자를 넘을 수 없습니다.").arg(maximumUserNameLength);
        return false;
    }
    if (password.size() < minimumPasswordLength) {
        error = QStringLiteral("비밀번호는 %1자 이상이어야 합니다.").arg(minimumPasswordLength);
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
