#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include "signingkeyrecovery.h"

#include <unistd.h>

namespace
{
constexpr auto PacmanPath = "/usr/bin/pacman";
constexpr auto StatePath = "/etc/pacman.d/flufflinux-update-state.json";
constexpr auto LogPath = "/etc/pacman.d/flufflinux-update.log";
constexpr auto PacmanLogPath = "/var/log/pacman.log";
constexpr auto CachePath = "/var/cache/pacman/pkg";
constexpr auto LockPath = "/var/lib/pacman/db.lck";
constexpr auto KeyRecoveryLockPath = "/run/flufflinux-update-key-recovery.lock";
constexpr auto FingerprintUrl =
    "https://fluffnet.org/flufflinux-fnrepo/packages/"
    "flufflinux-signing-key.fingerprint";
constexpr auto CertificateUrl =
    "https://fluffnet.org/flufflinux-fnrepo/packages/"
    "flufflinux-signing-key.asc";

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

void appendLog(const QString &text)
{
    QFile file(QString::fromLatin1(LogPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(text.toUtf8());
}

QString humanSpeed(qint64 bytesPerSecond)
{
    const double value = static_cast<double>(bytesPerSecond);
    if (bytesPerSecond >= 1024 * 1024) {
        return QString::number(value / (1024.0 * 1024.0), 'f', 1)
            + QStringLiteral(" MiB/s");
    }
    if (bytesPerSecond >= 1024) {
        return QString::number(value / 1024.0, 'f', 1) + QStringLiteral(" KiB/s");
    }
    return QString::number(bytesPerSecond) + QStringLiteral(" B/s");
}

class UpdateWorker final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateWorker(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_state = readState();
        m_state[QStringLiteral("worker_pid")] = static_cast<qint64>(getpid());
        QFile log(QString::fromLatin1(LogPath));
        if (log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            log.write("Fluff Linux Update diagnostic log\n");
            log.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
            log.write("\n\n");
        }
        determinePackages();
    }

private:
    QByteArray fetchHttpsArtifact(const char *url, qsizetype maximumBytes,
                                  const QString &oversizedCategory,
                                  const QString &unavailableCategory,
                                  QString *failureCategory)
    {
        QNetworkAccessManager manager;
        const QUrl expectedUrl(QString::fromLatin1(url));
        QNetworkRequest request(expectedUrl);
        request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::SameOriginRedirectPolicy);
        request.setTransferTimeout(15000);
        QNetworkReply *reply = manager.get(request);
        QByteArray response;
        bool oversized = false;
        QEventLoop eventLoop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(reply, &QNetworkReply::readyRead, &eventLoop, [&] {
            response += reply->readAll();
            if (response.size() > maximumBytes) {
                oversized = true;
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, &eventLoop,
                &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
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

    SigningKeyCommandResult runSigningKeyCommand(
        const QString &program, const QStringList &arguments)
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

    bool recoverSigningKey(const QString &output, int pacmanExitStatus,
                           const std::function<void()> &retry)
    {
        const QString repository =
            SigningKeyRecovery::repositoryName(output);
        if (SigningKeyRecovery::containsUnknownKeyReport(output)
            && repository != QStringLiteral("fluffnet")) {
            failSigningKey(
                repository,
                repository.isEmpty()
                    ? QStringLiteral("repository-unidentified")
                    : QStringLiteral("repository-not-fluffnet"),
                {}, {}, SigningKeyRecovery::requestedFingerprint(output),
                pacmanExitStatus);
            return true;
        }
        const QString requested =
            SigningKeyRecovery::requestedFingerprint(output);
        if (requested.isEmpty()) {
            if (SigningKeyRecovery::containsUnknownKeyReport(output)) {
                failSigningKey(
                    repository,
                    QStringLiteral("requested-fingerprint-invalid"), {}, {},
                    {}, pacmanExitStatus);
                return true;
            }
            return false;
        }
        if (m_attemptedSigningKeys.contains(requested)) {
            failSigningKey(repository,
                           QStringLiteral("recovery-retry-failed"), {}, {},
                           requested, pacmanExitStatus);
            return true;
        }
        m_attemptedSigningKeys.insert(requested);

        QLockFile recoveryLock(QString::fromLatin1(KeyRecoveryLockPath));
        recoveryLock.setStaleLockTime(0);
        if (!recoveryLock.tryLock(100)) {
            failSigningKey(repository,
                           QStringLiteral("recovery-already-running"), {}, {},
                           requested, pacmanExitStatus);
            return true;
        }

        SigningKeyRecovery recovery(
            SigningKeyRecoveryConfig{},
            [this](QString *category) {
                return fetchOfficialFingerprint(category);
            },
            [this](QString *category) {
                return fetchOfficialCertificate(category);
            },
            [this](const QString &program, const QStringList &arguments) {
                return runSigningKeyCommand(program, arguments);
            });
        const SigningKeyRecoveryResult result = recovery.recover(output);
        if (!result.recovered) {
            failSigningKey(result.repository, result.category,
                           result.expectedFingerprint,
                           result.receivedFingerprint,
                           result.requestedFingerprint, pacmanExitStatus);
            return true;
        }

        appendLog(QStringLiteral("\n[verified repository signing key imported] "
                                 "%1\n")
                      .arg(result.receivedFingerprint));
        m_state[QStringLiteral("security_repository")] = result.repository;
        m_state.remove(QStringLiteral("security_failure_category"));
        m_state.remove(QStringLiteral("security_expected_fingerprint"));
        m_state.remove(QStringLiteral("security_received_fingerprint"));
        m_state.remove(QStringLiteral("security_requested_fingerprint"));
        m_state.remove(QStringLiteral("security_pacman_exit_status"));
        m_state[QStringLiteral("recovery_notice_type")] =
            QStringLiteral("signing-key-verified");
        m_state[QStringLiteral("recovery_notice_id")] =
            QDateTime::currentMSecsSinceEpoch();
        writeState(m_state);
        QTimer::singleShot(300, this, retry);
        return true;
    }

    void failSigningKey(const QString &repository, const QString &category,
                        const QString &expected,
                        const QString &received, const QString &requested,
                        int pacmanExitStatus)
    {
        m_state[QStringLiteral("phase")] = QStringLiteral("failed");
        m_state[QStringLiteral("error")] =
            QStringLiteral("SIGNING_KEY_VERIFICATION_FAILED");
        m_state[QStringLiteral("speed")] = QString();
        m_state[QStringLiteral("security_repository")] = repository.left(64);
        m_state[QStringLiteral("security_failure_category")] = category.left(128);
        m_state[QStringLiteral("security_expected_fingerprint")] = expected.left(40);
        m_state[QStringLiteral("security_received_fingerprint")] = received.left(40);
        m_state[QStringLiteral("security_requested_fingerprint")] = requested.left(40);
        m_state[QStringLiteral("security_pacman_exit_status")] = pacmanExitStatus;
        writeState(m_state);
        QCoreApplication::quit();
    }

    void determinePackages()
    {
        m_state[QStringLiteral("phase")] = QStringLiteral("starting");
        writeState(m_state);

        auto *process = new QProcess(this);
        process->setProgram(QString::fromLatin1(PacmanPath));
        process->setArguments({QStringLiteral("-Syup"), QStringLiteral("--noconfirm"),
                               QStringLiteral("--print-format"),
                               QStringLiteral("%l|FLUFF|%s")});
        process->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        process->setProcessEnvironment(environment);
        connect(process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail(QStringLiteral("TRANSACTION_PREPARE_FAILED"));
            }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, process](int exitCode, QProcess::ExitStatus status) {
            const QString output = QString::fromLocal8Bit(process->readAll());
            appendLog(QStringLiteral("[transaction preparation]\n") + output
                      + QStringLiteral("\n"));
            process->deleteLater();
            if (status != QProcess::NormalExit || exitCode != 0) {
                if (recoverSigningKey(output, exitCode,
                                      [this] { determinePackages(); })) {
                    return;
                }
                fail(QStringLiteral("TRANSACTION_PREPARE_FAILED"));
                return;
            }

            const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const int separator =
                    line.lastIndexOf(QStringLiteral("|FLUFF|"));
                if (separator <= 0) {
                    continue;
                }
                const QUrl url(line.left(separator).trimmed());
                const QString name = url.fileName();
                bool validSize = false;
                const qint64 size =
                    line.mid(separator + 7).trimmed().toLongLong(&validSize);
                if (url.isValid() && name.contains(QStringLiteral(".pkg.tar."))
                    && !name.endsWith(QStringLiteral(".sig"))
                    && validSize && size >= 0) {
                    m_packages.insert(name);
                    m_packageSizes.insert(name, size);
                }
            }
            for (auto iterator = m_packageSizes.cbegin();
                 iterator != m_packageSizes.cend(); ++iterator) {
                m_totalDownloadBytes += iterator.value();
            }
            m_state[QStringLiteral("total_packages")] = m_packages.size();
            m_state[QStringLiteral("total_download_bytes")] =
                m_totalDownloadBytes;
            startDownload();
        });
        process->start();
    }

    qint64 cachedBytes() const
    {
        qint64 bytes = 0;
        for (const QString &package : m_packages) {
            const qint64 expected = m_packageSizes.value(package);
            // A zero download size means pacman already has the complete
            // archive in its cache. Pacman excludes that archive from the
            // transaction's Total Download Size, so it must also be excluded
            // from FLU's downloaded-byte counter.
            if (expected <= 0) {
                continue;
            }
            const QFileInfo complete(QString::fromLatin1(CachePath) + QLatin1Char('/') + package);
            const QFileInfo partial(complete.filePath() + QStringLiteral(".part"));
            if (complete.exists()) {
                bytes += qMin(complete.size(), expected);
            } else if (partial.exists()) {
                bytes += qMin(partial.size(), expected);
            }
        }
        return bytes;
    }

    int completedDownloads() const
    {
        int completed = 0;
        for (const QString &package : m_packages) {
            if (QFileInfo::exists(QString::fromLatin1(CachePath)
                                  + QLatin1Char('/') + package)) {
                ++completed;
            }
        }
        return completed;
    }

    void startDownload()
    {
        appendLog(QStringLiteral("\n[download]\n"));
        m_downloadOutput.clear();
        const qint64 initialBytes = cachedBytes();
        m_state[QStringLiteral("phase")] = QStringLiteral("downloading");
        m_state[QStringLiteral("completed_packages")] = completedDownloads();
        m_state[QStringLiteral("downloaded_bytes")] = initialBytes;
        m_state[QStringLiteral("progress")] = downloadProgress(initialBytes);
        writeState(m_state);

        m_lastBytes = initialBytes;
        m_speedTimer = new QTimer(this);
        m_speedTimer->setInterval(1000);
        connect(m_speedTimer, &QTimer::timeout, this, [this] {
            const qint64 bytes = cachedBytes();
            const qint64 delta = qMax<qint64>(0, bytes - m_lastBytes);
            m_lastBytes = bytes;
            m_recentByteDeltas.append(delta);
            while (m_recentByteDeltas.size() > 3) {
                m_recentByteDeltas.removeFirst();
            }
            qint64 averageDelta = 0;
            for (const qint64 sample : m_recentByteDeltas) {
                averageDelta += sample;
            }
            if (!m_recentByteDeltas.isEmpty()) {
                averageDelta /= m_recentByteDeltas.size();
            }
            const int completed = completedDownloads();
            m_state[QStringLiteral("completed_packages")] = completed;
            m_state[QStringLiteral("downloaded_bytes")] = bytes;
            m_state[QStringLiteral("progress")] = downloadProgress(bytes);
            m_state[QStringLiteral("speed")] = humanSpeed(averageDelta);
            writeState(m_state);
        });
        m_speedTimer->start();

        m_process = new QProcess(this);
        m_process->setProgram(QString::fromLatin1(PacmanPath));
        m_process->setArguments({QStringLiteral("-Syu"), QStringLiteral("--downloadonly"),
                                 QStringLiteral("--noconfirm"), QStringLiteral("--color"),
                                 QStringLiteral("never")});
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        m_process->setProcessEnvironment(environment);
        connect(m_process, &QProcess::readyRead, this, [this] {
            const QString chunk = QString::fromLocal8Bit(m_process->readAll());
            m_downloadOutput += chunk;
            appendLog(chunk);
        });
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail(QStringLiteral("DOWNLOAD_FAILED"));
            }
        });
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus status) {
            m_speedTimer->stop();
            m_speedTimer->deleteLater();
            m_speedTimer = nullptr;
            const QString trailing = QString::fromLocal8Bit(m_process->readAll());
            m_downloadOutput += trailing;
            appendLog(trailing);
            if (status != QProcess::NormalExit || exitCode != 0) {
                m_process->deleteLater();
                m_process = nullptr;
                if (recoverSigningKey(m_downloadOutput, exitCode,
                                      [this] { startDownload(); })) {
                    return;
                }
                const QString lower = m_downloadOutput.toLower();
                const bool connectionFailure =
                    lower.contains(QStringLiteral("failed retrieving file"))
                    || lower.contains(QStringLiteral("could not resolve host"))
                    || lower.contains(QStringLiteral("failed to connect"))
                    || lower.contains(QStringLiteral("connection timed out"))
                    || lower.contains(QStringLiteral("network is unreachable"));
                fail(connectionFailure
                         ? QStringLiteral("DOWNLOAD_CONNECTION_FAILED")
                         : QStringLiteral("DOWNLOAD_FAILED"));
                return;
            }
            m_process->deleteLater();
            m_process = nullptr;
            startInstall();
        });
        m_process->start();
    }

    void startInstall()
    {
        appendLog(QStringLiteral("\n[installation]\n"));
        m_state[QStringLiteral("phase")] = QStringLiteral("installing");
        m_state[QStringLiteral("completed_packages")] = 0;
        m_state[QStringLiteral("progress")] = 0;
        m_state[QStringLiteral("speed")] = QString();
        m_fullInstallOutput.clear();
        m_installOutput.clear();
        writeState(m_state);

        m_pacmanLogOffset = QFileInfo(QString::fromLatin1(PacmanLogPath)).size();
        m_installProgressTimer = new QTimer(this);
        m_installProgressTimer->setInterval(250);
        connect(m_installProgressTimer, &QTimer::timeout,
                this, [this] { readPacmanInstallLog(); });
        m_installProgressTimer->start();

        m_process = new QProcess(this);
        m_process->setProgram(QString::fromLatin1(PacmanPath));
        m_process->setArguments({QStringLiteral("-Syu"), QStringLiteral("--noconfirm"),
                                 QStringLiteral("--color"), QStringLiteral("never")});
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        m_process->setProcessEnvironment(environment);
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail(QStringLiteral("INSTALL_FAILED"));
            }
        });
        connect(m_process, &QProcess::readyRead, this, [this] {
            processInstallChunk(QString::fromLocal8Bit(m_process->readAll()));
        });
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus status) {
            processInstallChunk(
                QString::fromLocal8Bit(m_process->readAll()) + QLatin1Char('\n'));
            readPacmanInstallLog();
            m_installProgressTimer->stop();
            m_installProgressTimer->deleteLater();
            m_installProgressTimer = nullptr;
            if (status != QProcess::NormalExit || exitCode != 0) {
                m_process->deleteLater();
                m_process = nullptr;
                if (recoverSigningKey(m_fullInstallOutput, exitCode,
                                      [this] { startInstall(); })) {
                    return;
                }
                if (recoverFileConflict(m_fullInstallOutput)) {
                    return;
                }
                fail(QStringLiteral("INSTALL_FAILED"));
                return;
            }
            m_state[QStringLiteral("phase")] = QStringLiteral("complete");
            m_state[QStringLiteral("progress")] = 100;
            m_state[QStringLiteral("completed_packages")]
                = m_state.value(QStringLiteral("total_packages"));
            m_state[QStringLiteral("error")] = QString();
            writeState(m_state);
            QCoreApplication::quit();
        });
        m_process->start();
    }

    void readPacmanInstallLog()
    {
        QFile log(QString::fromLatin1(PacmanLogPath));
        if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;
        }
        if (log.size() < m_pacmanLogOffset) {
            m_pacmanLogOffset = 0;
        }
        if (!log.seek(m_pacmanLogOffset)) {
            return;
        }
        const QString appended = QString::fromUtf8(log.readAll());
        m_pacmanLogOffset = log.pos();
        const QRegularExpression operation(
            QStringLiteral("\\[ALPM\\]\\s+"
                           "(?:upgraded|installed|downgraded|reinstalled|removed)\\s+"));
        m_pacmanLogBuffer += appended;
        int newlyCompleted = 0;
        int newline = -1;
        while ((newline = m_pacmanLogBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = m_pacmanLogBuffer.left(newline);
            m_pacmanLogBuffer.remove(0, newline + 1);
            if (operation.match(line).hasMatch()) {
                ++newlyCompleted;
            }
        }
        if (newlyCompleted == 0) {
            return;
        }
        m_logCompleted += newlyCompleted;
        publishInstallProgress(m_logCompleted, 0);
    }

    void processInstallChunk(const QString &chunk)
    {
        appendLog(chunk);
        m_fullInstallOutput += chunk;
        m_installOutput += chunk;
        m_installOutput.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        const QRegularExpression expression(
            QStringLiteral("\\(\\s*(\\d+)\\s*/\\s*(\\d+)\\s*\\)\\s+"
                           "(?:upgrading|downgrading|installing|reinstalling|removing)"));
        int newline = -1;
        while ((newline = m_installOutput.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = m_installOutput.left(newline);
            m_installOutput.remove(0, newline + 1);
            const auto match = expression.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            const int completed = match.captured(1).toInt();
            const int total = match.captured(2).toInt();
            if (total > 0) {
                m_outputCompleted = qMax(m_outputCompleted, completed);
                publishInstallProgress(m_outputCompleted, total);
            }
        }
    }

    bool recoverFileConflict(const QString &output)
    {
        if (m_fileRecoveryCount >= 25) {
            appendLog(QStringLiteral("\n[file recovery limit reached]\n"));
            return false;
        }
        const QRegularExpression conflict(
            QStringLiteral("(?:^|\\n)[^:\\n]+:\\s+(/[^\\n]+?)\\s+exists in filesystem"));
        const auto match = conflict.match(output);
        if (!match.hasMatch()) {
            return false;
        }
        const QString original = QDir::cleanPath(match.captured(1).trimmed());
        if (!original.startsWith(QLatin1Char('/')) || original == QStringLiteral("/")) {
            return false;
        }
        const QFileInfo originalInfo(original);
        if (!originalInfo.exists() && !originalInfo.isSymLink()) {
            return false;
        }

        QString preserved;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int suffix = QRandomGenerator::global()->bounded(10000, 100000);
            const QString candidate = original + QStringLiteral(".preupdate")
                + QString::number(suffix);
            if (!QFileInfo::exists(candidate)) {
                preserved = candidate;
                break;
            }
        }
        if (preserved.isEmpty() || !QFile::rename(original, preserved)) {
            appendLog(QStringLiteral("\n[file recovery failed] ") + original
                      + QLatin1Char('\n'));
            return false;
        }

        ++m_fileRecoveryCount;
        appendLog(QStringLiteral("\n[file conflict recovered]\n") + original
                  + QStringLiteral(" -> ") + preserved + QLatin1Char('\n'));
        m_state[QStringLiteral("recovery_original_file")] = original;
        m_state[QStringLiteral("recovery_preserved_file")] = preserved;
        m_state[QStringLiteral("recovery_notice_id")] =
            QDateTime::currentMSecsSinceEpoch();
        m_state[QStringLiteral("phase")] = QStringLiteral("installing");
        m_state[QStringLiteral("progress")] = 0;
        m_state[QStringLiteral("completed_packages")] = 0;
        // Pacman normally removes its lock after a failed transaction.
        // The worker may clear a stale lock before retrying the installation.
        if (QFile::exists(QString::fromLatin1(LockPath))) {
            QFile::remove(QString::fromLatin1(LockPath));
        }
        writeState(m_state);
        QTimer::singleShot(300, this, [this] { startInstall(); });
        return true;
    }

    void publishInstallProgress(int completed, int reportedTotal)
    {
        if (reportedTotal > 0) {
            m_state[QStringLiteral("total_packages")] = reportedTotal;
        }
        const int total =
            m_state.value(QStringLiteral("total_packages")).toInt();
        const int visibleCompleted =
            qMax(m_state.value(QStringLiteral("completed_packages")).toInt(),
                 completed);
        m_state[QStringLiteral("completed_packages")] =
            total > 0 ? qMin(visibleCompleted, total) : visibleCompleted;
        if (total > 0) {
            m_state[QStringLiteral("progress")] =
                100.0 * qMin(visibleCompleted, total) / total;
        }
        writeState(m_state);
    }

    void fail(const QString &error)
    {
        m_state[QStringLiteral("phase")] = QStringLiteral("failed");
        m_state[QStringLiteral("error")] = error;
        m_state[QStringLiteral("speed")] = QString();
        writeState(m_state);
        QCoreApplication::quit();
    }

    double downloadProgress(qint64 bytes) const
    {
        if (m_totalDownloadBytes > 0) {
            return qBound(0.0,
                          100.0 * static_cast<double>(bytes)
                              / static_cast<double>(m_totalDownloadBytes),
                          100.0);
        }
        return m_packages.isEmpty()
            ? 100.0
            : 100.0 * completedDownloads() / m_packages.size();
    }

    QJsonObject m_state;
    QSet<QString> m_packages;
    QHash<QString, qint64> m_packageSizes;
    QProcess *m_process = nullptr;
    QTimer *m_speedTimer = nullptr;
    QTimer *m_installProgressTimer = nullptr;
    qint64 m_lastBytes = 0;
    qint64 m_totalDownloadBytes = 0;
    QList<qint64> m_recentByteDeltas;
    QString m_installOutput;
    QString m_downloadOutput;
    QString m_fullInstallOutput;
    QString m_pacmanLogBuffer;
    qint64 m_pacmanLogOffset = 0;
    int m_logCompleted = 0;
    int m_outputCompleted = 0;
    int m_fileRecoveryCount = 0;
    QSet<QString> m_attemptedSigningKeys;
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    UpdateWorker worker;
    return application.exec();
}

#include "updateworker.moc"
