#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QEventLoop>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "signingkeyrecovery.h"

#include <csignal>
#include <iostream>

#include <unistd.h>

namespace
{
constexpr auto PacmanPath = "/usr/bin/pacman";
constexpr auto SystemctlPath = "/usr/bin/systemctl";
constexpr auto DatabasePrefix = "/tmp/flufflinux-checkupdates-";
constexpr auto StatePath = "/etc/pacman.d/flufflinux-update-state.json";
constexpr auto LastUpdatePath = "/etc/pacman.d/lastupdate.json";
constexpr auto LastUpdateKey = "last_successful_system_update";
constexpr auto LockPath = "/var/lib/pacman/db.lck";
constexpr auto ProtectionPath =
    "/etc/pacman.d/flufflinux-update-package-protection.json";
constexpr auto KeyRecoveryLockPath = "/run/flufflinux-update-key-recovery.lock";
constexpr auto FingerprintUrl =
    "https://fluffnet.org/flufflinux-fnrepo/packages/"
    "flufflinux-signing-key.fingerprint";
constexpr auto CertificateUrl =
    "https://fluffnet.org/flufflinux-fnrepo/packages/"
    "flufflinux-signing-key.asc";

enum class RemovalClass { Protected, Warning, Autoremove };

bool pacmanRunning();

QByteArray fetchHttpsArtifact(const char *url, qsizetype maximumBytes,
                              const QString &oversizedCategory,
                              const QString &unavailableCategory,
                              QString *failureCategory)
{
    QNetworkAccessManager manager;
    const QUrl expectedUrl(QString::fromLatin1(url));
    QNetworkRequest request(expectedUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = manager.get(request);
    QByteArray response;
    bool oversized = false;
    QEventLoop eventLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::readyRead, &eventLoop, [&] {
        response += reply->readAll();
        if (response.size() > maximumBytes) {
            oversized = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &eventLoop,
                     &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout.start(15000);
    eventLoop.exec();
    response += reply->readAll();
    if (oversized) {
        *failureCategory = oversizedCategory;
        reply->deleteLater();
        return {};
    }
    const bool secureResponse = reply->url().scheme() == QStringLiteral("https")
        && reply->url().host() == expectedUrl.host();
    const bool succeeded = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    if (!secureResponse || !succeeded) {
        *failureCategory = unavailableCategory;
        return {};
    }
    return response;
}

QByteArray fetchOfficialFingerprint(QString *failureCategory)
{
    return fetchHttpsArtifact(
        FingerprintUrl, 4096,
        QStringLiteral("fingerprint-response-oversized"),
        QStringLiteral("fingerprint-endpoint-unavailable"), failureCategory);
}

QByteArray fetchOfficialCertificate(QString *failureCategory)
{
    return fetchHttpsArtifact(
        CertificateUrl, 256 * 1024,
        QStringLiteral("certificate-response-oversized"),
        QStringLiteral("certificate-endpoint-unavailable"), failureCategory);
}

SigningKeyCommandResult runSigningKeyCommand(const QString &program,
                                              const QStringList &arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start();
    SigningKeyCommandResult result;
    result.started = process.waitForStarted(5000);
    if (!result.started) {
        return result;
    }
    result.finished = process.waitForFinished(30000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.output = process.readAll();
    result.exitCode = process.exitStatus() == QProcess::NormalExit
        ? process.exitCode() : -1;
    return result;
}

int recoverSigningKeyForCheck(const QString &encodedOutput)
{
    const QByteArray decoded = QByteArray::fromBase64(
        encodedOutput.toLatin1(), QByteArray::Base64UrlEncoding);
    if (decoded.isEmpty() || decoded.size() > 1024 * 1024) {
        std::cout << "FLU_SIGNING_KEY_FAILURE:|check-output-invalid||||1\n";
        return 24;
    }
    const QString output = QString::fromUtf8(decoded);
    const QString repository = SigningKeyRecovery::repositoryName(output);
    const QString requested = SigningKeyRecovery::requestedFingerprint(output);
    if (repository != QStringLiteral("fluffnet")) {
        const QString category = repository.isEmpty()
            ? QStringLiteral("repository-unidentified")
            : QStringLiteral("repository-not-fluffnet");
        std::cout << "FLU_SIGNING_KEY_FAILURE:"
                  << repository.toStdString() << '|'
                  << category.toStdString() << "|||"
                  << requested.toStdString() << "|1\n";
        return 24;
    }
    QLockFile recoveryLock(QString::fromLatin1(KeyRecoveryLockPath));
    recoveryLock.setStaleLockTime(0);
    if (!recoveryLock.tryLock(100)) {
        std::cout << "FLU_SIGNING_KEY_FAILURE:"
                  << repository.toStdString()
                  << "|recovery-already-running|||"
                  << requested.toStdString() << "|1\n";
        return 24;
    }
    SigningKeyRecovery recovery(SigningKeyRecoveryConfig{},
                                fetchOfficialFingerprint,
                                fetchOfficialCertificate,
                                runSigningKeyCommand);
    const SigningKeyRecoveryResult result = recovery.recover(output);
    if (result.recovered) {
        return 0;
    }
    std::cout << "FLU_SIGNING_KEY_FAILURE:"
              << result.repository.toStdString() << '|'
              << result.category.toStdString() << '|'
              << result.expectedFingerprint.toStdString() << '|'
              << result.receivedFingerprint.toStdString() << '|'
              << result.requestedFingerprint.toStdString() << "|1\n";
    return 24;
}

struct ProtectionPolicy {
    QSet<QString> protectedPackages;
    QSet<QString> warningPackages;
    bool valid = false;
};

ProtectionPolicy readProtectionPolicy()
{
    QFile file(QString::fromLatin1(ProtectionPath));
    if (!file.open(QIODevice::ReadOnly)) {
        // Fail closed if the packaged policy disappeared: never silently
        // remove an unknown package without the administrator's policy file.
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return {};
    }
    ProtectionPolicy policy;
    const auto append = [](const QJsonArray &array, QSet<QString> &target) {
        for (const QJsonValue &value : array) {
            const QString package = value.toString();
            if (!package.isEmpty()) {
                target.insert(package);
            }
        }
    };
    append(document.object().value(QStringLiteral("protected")).toArray(),
           policy.protectedPackages);
    append(document.object().value(QStringLiteral("warning")).toArray(),
           policy.warningPackages);
    policy.valid = true;
    return policy;
}

RemovalClass classifyRemoval(const ProtectionPolicy &policy,
                             const QString &package)
{
    if (!policy.valid) {
        return RemovalClass::Protected;
    }
    if (policy.protectedPackages.contains(package)) {
        return RemovalClass::Protected;
    }
    if (policy.warningPackages.contains(package)) {
        return RemovalClass::Warning;
    }
    return RemovalClass::Autoremove;
}

bool validPackageName(const QString &package)
{
    static const QRegularExpression valid(
        QStringLiteral("^[A-Za-z0-9@._+:-]+$"));
    return valid.match(package).hasMatch();
}

QString installedPackageVersion(const QString &package)
{
    QProcess query;
    query.setProgram(QString::fromLatin1(PacmanPath));
    query.setArguments({QStringLiteral("-Q"), package});
    query.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    query.setProcessEnvironment(environment);
    query.start();
    if (!query.waitForStarted() || !query.waitForFinished(5000)
        || query.exitStatus() != QProcess::NormalExit || query.exitCode() != 0) {
        return {};
    }
    const QStringList fields = QString::fromLocal8Bit(query.readAll())
        .trimmed().split(QRegularExpression(QStringLiteral("\\s+")));
    return fields.size() >= 2 ? fields.constLast() : QString{};
}

QString syncPackageVersion(const QString &database, const QString &package)
{
    QProcess query;
    query.setProgram(QString::fromLatin1(PacmanPath));
    query.setArguments({QStringLiteral("--dbpath"), database,
                        QStringLiteral("-Si"), package});
    query.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    query.setProcessEnvironment(environment);
    query.start();
    if (!query.waitForStarted() || !query.waitForFinished(5000)
        || query.exitStatus() != QProcess::NormalExit || query.exitCode() != 0) {
        return {};
    }
    const QString output = QString::fromLocal8Bit(query.readAll());
    const QRegularExpression versionLine(
        QStringLiteral("(?:^|\\n)Version\\s*:\\s*(\\S+)"));
    const auto match = versionLine.match(output);
    return match.hasMatch() ? match.captured(1) : QString{};
}

QList<QPair<QString, QString>> plannedPackages(const QString &database)
{
    QProcess query;
    query.setProgram(QString::fromLatin1(PacmanPath));
    query.setArguments({QStringLiteral("--dbpath"), database,
                        QStringLiteral("-Sup"), QStringLiteral("--noconfirm"),
                        QStringLiteral("--print-format"),
                        QStringLiteral("%n|%v")});
    query.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    query.setProcessEnvironment(environment);
    query.start();
    if (!query.waitForStarted() || !query.waitForFinished(15000)
        || query.exitStatus() != QProcess::NormalExit || query.exitCode() != 0) {
        return {};
    }

    QList<QPair<QString, QString>> result;
    const QStringList lines = QString::fromLocal8Bit(query.readAll())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const qsizetype separator = line.indexOf(QLatin1Char('|'));
        if (separator <= 0 || separator == line.size() - 1) {
            continue;
        }
        const QString name = line.left(separator);
        const QString version = line.mid(separator + 1);
        if (validPackageName(name)
            && !version.contains(QRegularExpression(QStringLiteral("\\s")))) {
            result.append({name, version});
        }
    }
    return result;
}

int removePackage(const QString &package)
{
    if (!validPackageName(package)) {
        std::cerr << "INVALID_PACKAGE\n";
        return 2;
    }
    if (pacmanRunning()) {
        std::cerr << "PACMAN_RUNNING\n";
        return 3;
    }
    if (QFile::exists(QString::fromLatin1(LockPath))) {
        QFile::remove(QString::fromLatin1(LockPath));
    }
    QProcess pacman;
    pacman.setProgram(QString::fromLatin1(PacmanPath));
    pacman.setArguments({QStringLiteral("-Rdd"), QStringLiteral("--noconfirm"),
                         package});
    pacman.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    pacman.setProcessEnvironment(environment);
    pacman.start();
    if (!pacman.waitForStarted() || !pacman.waitForFinished(-1)) {
        std::cerr << "PACKAGE_REMOVAL_FAILED\n";
        return 1;
    }
    const QByteArray output = pacman.readAll();
    std::cout.write(output.constData(), output.size());
    return pacman.exitStatus() == QProcess::NormalExit ? pacman.exitCode() : 1;
}

bool pacmanRunning()
{
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        entry.toUInt(&numeric);
        if (!numeric) {
            continue;
        }
        QFile comm(QStringLiteral("/proc/") + entry + QStringLiteral("/comm"));
        if (comm.open(QIODevice::ReadOnly)
            && QString::fromLocal8Bit(comm.readAll()).trimmed() == QStringLiteral("pacman")) {
            return true;
        }
        const QString executable = QFile::symLinkTarget(
            QStringLiteral("/proc/") + entry + QStringLiteral("/exe"));
        if (executable == QString::fromLatin1(PacmanPath)) {
            return true;
        }
    }
    return false;
}

QJsonObject readState()
{
    QFile file(QString::fromLatin1(StatePath));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

bool writeState(const QJsonObject &state)
{
    QSaveFile file(QString::fromLatin1(StatePath));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    return file.commit();
}

int transactionSummary(const QString &requestedDatabase,
                       const QSet<QString> &approvedRemovals,
                       bool signingKeyRetry = false)
{
    Q_UNUSED(approvedRemovals);
    const QByteArray invokingUid = qgetenv("PKEXEC_UID");
    bool validUid = false;
    invokingUid.toUInt(&validUid);
    if (!validUid) {
        std::cerr << "flufflinux-update-helper: missing invoking user\n";
        return 2;
    }

    const QString expectedDatabase = QString::fromLatin1(DatabasePrefix)
        + QString::fromLatin1(invokingUid);
    if (requestedDatabase != expectedDatabase) {
        std::cerr << "flufflinux-update-helper: invalid database path\n";
        return 2;
    }

    // The transaction planner is intentionally terminated when FLU needs to
    // resolve a removal before retrying. A killed pacman process cannot clean
    // its temporary database lock itself. Because the path above is strictly
    // tied to the invoking user, and no real pacman process is active, this is
    // a stale lock owned by FLU's isolated check database and is safe to clear.
    if (pacmanRunning()) {
        std::cerr << "PACMAN_RUNNING\n";
        return 3;
    }
    const QString temporaryLock = QDir(requestedDatabase)
        .filePath(QStringLiteral("db.lck"));
    QFile::remove(temporaryLock);

    QProcess pacman;
    pacman.setProgram(QString::fromLatin1(PacmanPath));
    pacman.setArguments({QStringLiteral("--dbpath"), requestedDatabase,
                         QStringLiteral("-Su")});
    pacman.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    pacman.setProcessEnvironment(environment);
    pacman.start();

    if (!pacman.waitForStarted()) {
        std::cerr << "flufflinux-update-helper: could not start pacman\n";
        return 1;
    }

    const ProtectionPolicy policy = readProtectionPolicy();
    QByteArray output;
    qsizetype parsedThrough = 0;
    bool finalConfirmationAnswered = false;
    QList<QPair<QString, QString>> replacements;
    while (pacman.state() != QProcess::NotRunning) {
        pacman.waitForReadyRead(100);
        output += pacman.readAll();
        const QString text = QString::fromLocal8Bit(output);
        const QString unparsed = text.mid(parsedThrough);

        static const QRegularExpression replacement(
            QStringLiteral("Replace\\s+([A-Za-z0-9@._+:-]+)\\s+with\\s+(?:[A-Za-z0-9@._+:-]+/)?([A-Za-z0-9@._+:-]+)\\?\\s*\\[Y/n\\]"),
            QRegularExpression::CaseInsensitiveOption);
        const auto replacementMatch = replacement.match(unparsed);
        if (replacementMatch.hasMatch()) {
            replacements.append({replacementMatch.captured(1),
                                 replacementMatch.captured(2)});
            pacman.write("y\n");
            pacman.waitForBytesWritten();
            parsedThrough += replacementMatch.capturedEnd();
            continue;
        }

        static const QRegularExpression removal(
            QStringLiteral("Remove\\s+([A-Za-z0-9@._+:-]+)\\?\\s*\\[y/N\\]"),
            QRegularExpression::CaseInsensitiveOption);
        const auto removalMatch = removal.match(unparsed);
        if (removalMatch.hasMatch()) {
            const QString package = removalMatch.captured(1);
            switch (classifyRemoval(policy, package)) {
            case RemovalClass::Protected:
                pacman.kill();
                pacman.waitForFinished();
                QFile::remove(temporaryLock);
                std::cout << output.constData();
                std::cout << "\nFLU_PROTECTED_REMOVAL:" << package.toStdString()
                          << "\n";
                return 20;
            case RemovalClass::Warning:
                pacman.kill();
                pacman.waitForFinished();
                QFile::remove(temporaryLock);
                std::cout << output.constData();
                std::cout << "\nFLU_WARNING_REMOVAL:" << package.toStdString()
                          << "\n";
                return 21;
            case RemovalClass::Autoremove:
                pacman.kill();
                pacman.waitForFinished();
                QFile::remove(temporaryLock);
                if (removePackage(package) != 0) {
                    std::cout.write(output.constData(), output.size());
                    std::cout << "\nFLU_AUTOREMOVE_FAILED:"
                              << package.toStdString() << "\n";
                    return 22;
                }
                std::cout << "FLU_AUTOREMOVED:" << package.toStdString()
                          << "\n";
                return 23;
            }
        }

        if (!finalConfirmationAnswered
            && unparsed.contains(QStringLiteral("Proceed with installation?"))) {
            // Planning mode accepts transaction questions but always refuses
            // pacman's final confirmation, so this command cannot install.
            pacman.write("n\n");
            pacman.closeWriteChannel();
            finalConfirmationAnswered = true;
        }
    }
    output += pacman.readAll();

    if (pacman.exitCode() != 0
        && SigningKeyRecovery::containsUnknownKeyReport(
            QString::fromLocal8Bit(output))) {
        SigningKeyRecoveryResult recoveryResult;
        recoveryResult.repository = SigningKeyRecovery::repositoryName(
            QString::fromLocal8Bit(output));
        recoveryResult.requestedFingerprint =
            SigningKeyRecovery::requestedFingerprint(
                QString::fromLocal8Bit(output));
        if (recoveryResult.repository != QStringLiteral("fluffnet")) {
            recoveryResult.category = recoveryResult.repository.isEmpty()
                ? QStringLiteral("repository-unidentified")
                : QStringLiteral("repository-not-fluffnet");
        } else if (signingKeyRetry) {
            recoveryResult.category = QStringLiteral("recovery-retry-failed");
        } else {
            QLockFile recoveryLock(QString::fromLatin1(KeyRecoveryLockPath));
            recoveryLock.setStaleLockTime(0);
            if (!recoveryLock.tryLock(100)) {
                recoveryResult.category =
                    QStringLiteral("recovery-already-running");
            } else {
                SigningKeyRecovery recovery(
                    SigningKeyRecoveryConfig{}, fetchOfficialFingerprint,
                    fetchOfficialCertificate,
                    runSigningKeyCommand);
                recoveryResult = recovery.recover(
                    QString::fromLocal8Bit(output));
            }
        }
        if (recoveryResult.recovered) {
            QFile::remove(temporaryLock);
            return transactionSummary(requestedDatabase, approvedRemovals, true);
        }
        std::cout << "FLU_SIGNING_KEY_FAILURE:"
                  << recoveryResult.repository.toStdString() << '|'
                  << recoveryResult.category.toStdString() << '|'
                  << recoveryResult.expectedFingerprint.toStdString() << '|'
                  << recoveryResult.receivedFingerprint.toStdString() << '|'
                  << recoveryResult.requestedFingerprint.toStdString() << '|'
                  << pacman.exitCode() << "\n";
        return 24;
    }

    for (const auto &[oldPackage, newPackage] : replacements) {
        const QString oldVersion = installedPackageVersion(oldPackage);
        const QString newVersion = syncPackageVersion(requestedDatabase,
                                                      newPackage);
        if (!oldVersion.isEmpty() && !newVersion.isEmpty()) {
            std::cout << "FLU_REPLACEMENT:" << oldPackage.toStdString() << '|'
                      << oldVersion.toStdString() << '|'
                      << newPackage.toStdString() << '|'
                      << newVersion.toStdString() << "\n";
        }
    }
    for (const auto &[package, version] : plannedPackages(requestedDatabase)) {
        std::cout << "FLU_PLANNED_PACKAGE:" << package.toStdString() << '|'
                  << version.toStdString() << "\n";
    }

    // A dependency break can abort before pacman asks an interactive removal
    // question. The package after "required by" is the installed blocker.
    if (pacman.exitCode() != 0) {
        const QString text = QString::fromLocal8Bit(output);
        const QRegularExpression requiredBy(
            QStringLiteral("required by\\s+([A-Za-z0-9@._+:-]+)"));
        auto iterator = requiredBy.globalMatch(text);
        QSet<QString> blockers;
        while (iterator.hasNext()) {
            blockers.insert(iterator.next().captured(1));
        }
        for (const QString &package : blockers) {
            const RemovalClass removalClass = classifyRemoval(policy, package);
            if (removalClass == RemovalClass::Protected) {
                std::cout.write(output.constData(), output.size());
                std::cout << "\nFLU_PROTECTED_REMOVAL:" << package.toStdString()
                          << "\n";
                return 20;
            }
            if (removalClass == RemovalClass::Warning) {
                std::cout.write(output.constData(), output.size());
                std::cout << "\nFLU_WARNING_DEPENDENCY:" << package.toStdString()
                          << "\n";
                return 21;
            }
            const int removalResult = removePackage(package);
            if (removalResult != 0) {
                std::cout.write(output.constData(), output.size());
                std::cout << "\nFLU_AUTOREMOVE_FAILED:" << package.toStdString()
                          << "\n";
                return 22;
            }
            std::cout << "FLU_AUTOREMOVED:" << package.toStdString()
                      << "\n";
            return 23;
        }
    }

    std::cout.write(output.constData(), output.size());
    std::cout.flush();
    return pacman.exitStatus() == QProcess::NormalExit ? pacman.exitCode() : 1;
}

int startInstallation(const QString &downloadSize, const QString &storageChange,
                      bool storageFreed, const QString &encodedUpdates)
{
    const QJsonObject previousState = readState();
    const QString previousPhase =
        previousState.value(QStringLiteral("phase")).toString();
    const qint64 previousWorker =
        previousState.value(QStringLiteral("worker_pid")).toInteger();
    if ((previousPhase == QStringLiteral("starting")
         || previousPhase == QStringLiteral("downloading")
         || previousPhase == QStringLiteral("installing"))
        && previousWorker > 1
        && ::kill(static_cast<pid_t>(previousWorker), 0) == 0) {
        std::cerr << "PACMAN_RUNNING\n";
        return 3;
    }

    if (pacmanRunning()) {
        std::cerr << "PACMAN_RUNNING\n";
        return 3;
    }

    // Fluff Linux deliberately removes an ownerless stale lock aggressively.
    if (QFile::exists(QString::fromLatin1(LockPath))) {
        QFile::remove(QString::fromLatin1(LockPath));
    }

    const QByteArray decodedUpdates = QByteArray::fromBase64(
        encodedUpdates.toLatin1(), QByteArray::Base64UrlEncoding);
    const QJsonDocument updatesDocument =
        QJsonDocument::fromJson(decodedUpdates);
    const QJsonArray updates = updatesDocument.isArray()
        ? updatesDocument.array() : QJsonArray{};

    QJsonObject state{
        {QStringLiteral("phase"), QStringLiteral("starting")},
        {QStringLiteral("progress"), 0},
        {QStringLiteral("completed_packages"), 0},
        {QStringLiteral("total_packages"), 0},
        {QStringLiteral("speed"), QString()},
        {QStringLiteral("download_size"), downloadSize.left(128)},
        {QStringLiteral("storage_change"), storageChange.left(128)},
        {QStringLiteral("storage_freed"), storageFreed},
        {QStringLiteral("updates"), updates},
        {QStringLiteral("error"), QString()},
    };
    if (!writeState(state)) {
        std::cerr << "STATE_WRITE_FAILED\n";
        return 1;
    }

    QProcess systemctl;
    systemctl.setProgram(QString::fromLatin1(SystemctlPath));
    systemctl.setArguments(
        {QStringLiteral("start"), QStringLiteral("flufflinux-update.service")});
    systemctl.setProcessChannelMode(QProcess::MergedChannels);
    systemctl.start();
    if (!systemctl.waitForStarted() || !systemctl.waitForFinished()
        || systemctl.exitStatus() != QProcess::NormalExit
        || systemctl.exitCode() != 0) {
        state[QStringLiteral("phase")] = QStringLiteral("failed");
        state[QStringLiteral("error")] = QStringLiteral("WORKER_START_FAILED");
        writeState(state);
        std::cerr << "WORKER_START_FAILED\n";
        return 1;
    }

    return 0;
}

int cancelInstallation()
{
    QJsonObject state = readState();
    if (state.value(QStringLiteral("phase")).toString() != QStringLiteral("downloading")) {
        std::cerr << "CANCEL_NOT_ALLOWED\n";
        return 4;
    }

    QProcess systemctl;
    systemctl.setProgram(QString::fromLatin1(SystemctlPath));
    systemctl.setArguments(
        {QStringLiteral("stop"), QStringLiteral("flufflinux-update.service")});
    systemctl.start();
    if (!systemctl.waitForStarted() || !systemctl.waitForFinished(5000)
        || systemctl.exitStatus() != QProcess::NormalExit
        || systemctl.exitCode() != 0) {
        std::cerr << "CANCEL_FAILED\n";
        return 1;
    }

    // systemctl stopping the worker also stops its pacman child. Remove any
    // lock left behind by that cancelled download, but never touch the lock
    // if a pacman process is still alive (including one started separately).
    if (pacmanRunning()) {
        std::cerr << "PACMAN_RUNNING\n";
        return 3;
    }
    if (QFile::exists(QString::fromLatin1(LockPath))
        && !QFile::remove(QString::fromLatin1(LockPath))) {
        std::cerr << "CANCEL_FAILED\n";
        return 1;
    }

    state[QStringLiteral("phase")] = QStringLiteral("cancelled");
    state[QStringLiteral("progress")] = 0;
    state[QStringLiteral("speed")] = QString();
    state[QStringLiteral("error")] = QString();
    writeState(state);
    return 0;
}

int recordCurrentUpdate()
{
    QJsonObject state;
    QFile existing(QString::fromLatin1(LastUpdatePath));
    if (existing.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(existing.readAll());
        if (document.isObject()) {
            state = document.object();
        }
    }

    state[QString::fromLatin1(LastUpdateKey)] =
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss t tt"));

    QSaveFile file(QString::fromLatin1(LastUpdatePath));
    if (!file.open(QIODevice::WriteOnly)) {
        std::cerr << "LAST_UPDATE_WRITE_FAILED\n";
        return 1;
    }
    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        std::cerr << "LAST_UPDATE_WRITE_FAILED\n";
        return 1;
    }
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc == 3
        && QString::fromLocal8Bit(argv[1])
            == QStringLiteral("--recover-signing-key")) {
        return recoverSigningKeyForCheck(QString::fromLocal8Bit(argv[2]));
    }
    if ((argc == 2 || argc == 3) && QString::fromLocal8Bit(argv[1]).startsWith(
            QString::fromLatin1(DatabasePrefix))) {
        QSet<QString> approved;
        if (argc == 3) {
            const QStringList names = QString::fromLocal8Bit(argv[2]).split(
                QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &name : names) {
                if (validPackageName(name)) {
                    approved.insert(name);
                }
            }
        }
        return transactionSummary(QString::fromLocal8Bit(argv[1]), approved);
    }

    if (argc == 6 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--install")) {
        return startInstallation(QString::fromLocal8Bit(argv[2]),
                                 QString::fromLocal8Bit(argv[3]),
                                 QString::fromLocal8Bit(argv[4]) == QStringLiteral("true"),
                                 QString::fromLocal8Bit(argv[5]));
    }

    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--cancel")) {
        return cancelInstallation();
    }

    if (argc == 3
        && QString::fromLocal8Bit(argv[1])
            == QStringLiteral("--remove-package")) {
        return removePackage(QString::fromLocal8Bit(argv[2]));
    }

    if (argc == 2
        && QString::fromLocal8Bit(argv[1])
            == QStringLiteral("--record-current-update")) {
        return recordCurrentUpdate();
    }

    std::cerr << "flufflinux-update-helper: invalid arguments\n";
    return 2;
}
