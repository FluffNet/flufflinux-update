#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <unistd.h>

namespace
{
constexpr auto PacmanPath = "/usr/bin/pacman";
constexpr auto StatePath = "/etc/pacman.d/flufflinux-update-state.json";
constexpr auto LogPath = "/etc/pacman.d/flufflinux-update.log";
constexpr auto PacmanLogPath = "/var/log/pacman.log";
constexpr auto CachePath = "/var/cache/pacman/pkg";

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
            const QFileInfo complete(QString::fromLatin1(CachePath) + QLatin1Char('/') + package);
            const QFileInfo partial(complete.filePath() + QStringLiteral(".part"));
            if (complete.exists()) {
                bytes += expected > 0 ? qMin(complete.size(), expected)
                                      : complete.size();
            } else if (partial.exists()) {
                bytes += expected > 0 ? qMin(partial.size(), expected)
                                      : partial.size();
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
            appendLog(QString::fromLocal8Bit(m_process->readAll()));
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
            appendLog(QString::fromLocal8Bit(m_process->readAll()));
            if (status != QProcess::NormalExit || exitCode != 0) {
                fail(QStringLiteral("DOWNLOAD_FAILED"));
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
            if (status != QProcess::NormalExit || exitCode != 0) {
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
    QString m_pacmanLogBuffer;
    qint64 m_pacmanLogOffset = 0;
    int m_logCompleted = 0;
    int m_outputCompleted = 0;
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    UpdateWorker worker;
    return application.exec();
}

#include "updateworker.moc"
