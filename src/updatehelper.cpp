#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QString>

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

int transactionSummary(const QString &requestedDatabase)
{
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

    // Summary mode can never approve a transaction.
    pacman.write("n\n");
    pacman.closeWriteChannel();
    pacman.waitForFinished(-1);

    const QByteArray output = pacman.readAll();
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
    if (argc == 2 && QString::fromLocal8Bit(argv[1]).startsWith(
            QString::fromLatin1(DatabasePrefix))) {
        return transactionSummary(QString::fromLocal8Bit(argv[1]));
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

    if (argc == 2
        && QString::fromLocal8Bit(argv[1])
            == QStringLiteral("--record-current-update")) {
        return recordCurrentUpdate();
    }

    std::cerr << "flufflinux-update-helper: invalid arguments\n";
    return 2;
}
