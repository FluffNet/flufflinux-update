#include "fluffupdates.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSize>
#include <QStandardPaths>
#include <QApplication>
#include <QWidget>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QTimer>

#include <unistd.h>

#include <utility>

namespace
{
constexpr auto StateFile = "/etc/pacman.d/lastupdate.json";
constexpr auto InstallStateFile = "/etc/pacman.d/flufflinux-update-state.json";
constexpr auto LastUpdateKey = "last_successful_system_update";

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
        if (QFile::symLinkTarget(QStringLiteral("/proc/") + entry + QStringLiteral("/exe"))
            == QStringLiteral("/usr/bin/pacman")) {
            return true;
        }
    }
    return false;
}

QDateTime parseLastUpdate(const QString &value)
{
    // lastupdate writes: 2026-07-20 14:30:00 IDT +0300. The numeric offset is
    // the unambiguous timezone component used for freshness calculations.
    if (value.size() < 24) {
        return {};
    }
    const QString dateAndOffset = value.left(19) + QLatin1Char(' ') + value.right(5);
    return QDateTime::fromString(dateAndOffset, QStringLiteral("yyyy-MM-dd HH:mm:ss t"));
}

QString friendlyCheckError(const QString &output)
{
    const QString lower = output.toLower();
    const QStringList networkErrors = {
        QStringLiteral("could not resolve host"),
        QStringLiteral("name or service not known"),
        QStringLiteral("temporary failure in name resolution"),
        QStringLiteral("network is unreachable"),
        QStringLiteral("failed to connect"),
        QStringLiteral("connection timed out"),
        QStringLiteral("resolving timed out"),
        QStringLiteral("connection reset by peer"),
        QStringLiteral("operation too slow"),
        QStringLiteral("ssl connection timeout"),
        QStringLiteral("recv failure"),
    };
    for (const QString &error : networkErrors) {
        if (lower.contains(error)) {
            return i18nd("kcm_fluffupdates", "No internet connection. Check your network and try again.");
        }
    }

    const QStringList repositoryErrors = {
        QStringLiteral("failed retrieving file"),
        QStringLiteral("failed to synchronize all databases"),
        QStringLiteral("failed to update database"),
        QStringLiteral("failed to download"),
        QStringLiteral("could not find database"),
        QStringLiteral("the requested url returned error"),
        QStringLiteral("too many errors from"),
    };
    for (const QString &error : repositoryErrors) {
        if (lower.contains(error)) {
            return i18nd("kcm_fluffupdates", "The repository servers could not be reached. Try again later or check your mirror configuration.");
        }
    }

    return i18nd("kcm_fluffupdates", "The update check failed.");
}

QString humanDataSize(qint64 bytes)
{
    const auto formatted = [](double value) {
        QString text = QString::number(value, 'f', 1);
        if (text.endsWith(QStringLiteral(".0"))) {
            text.chop(2);
        }
        return text;
    };
    if (bytes >= 1024LL * 1024 * 1024) {
        return formatted(static_cast<double>(bytes) / (1024.0 * 1024 * 1024))
            + QStringLiteral(" GiB");
    }
    if (bytes >= 1024LL * 1024) {
        return formatted(static_cast<double>(bytes) / (1024.0 * 1024))
            + QStringLiteral(" MiB");
    }
    if (bytes >= 1024) {
        return formatted(static_cast<double>(bytes) / 1024.0)
            + QStringLiteral(" KiB");
    }
    return QString::number(qMax<qint64>(0, bytes)) + QStringLiteral(" B");
}

QVariantList parseUpdatePackages(const QString &output)
{
    QVariantList packages;
    const QRegularExpression packageExpression(
        QStringLiteral("^\\s*(\\S+)\\s+(\\S+)\\s+->\\s+(\\S+)\\s*$"));
    const QStringList lines =
        output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        const auto match = packageExpression.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        packages.append(QVariantMap{
            {QStringLiteral("name"), match.captured(1)},
            {QStringLiteral("currentVersion"), match.captured(2)},
            {QStringLiteral("newVersion"), match.captured(3)},
        });
    }

    return packages;
}

bool systemBatteryIsLow()
{
    const QDir powerSupplies(QStringLiteral("/sys/class/power_supply"));
    const QStringList entries =
        powerSupplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &entry : entries) {
        const QString path = powerSupplies.filePath(entry);
        QFile typeFile(path + QStringLiteral("/type"));
        QFile capacityFile(path + QStringLiteral("/capacity"));
        QFile statusFile(path + QStringLiteral("/status"));

        if (!typeFile.open(QIODevice::ReadOnly)
            || QString::fromLatin1(typeFile.readAll()).trimmed()
                != QStringLiteral("Battery")
            || !capacityFile.open(QIODevice::ReadOnly)) {
            continue;
        }

        bool validCapacity = false;
        const int capacity =
            QString::fromLatin1(capacityFile.readAll()).trimmed().toInt(
                &validCapacity);
        QString status;
        if (statusFile.open(QIODevice::ReadOnly)) {
            status = QString::fromLatin1(statusFile.readAll()).trimmed();
        }

        // A plugged-in laptop does not need a reminder. Linux reports a
        // battery that is actively powering the system as "Discharging".
        if (validCapacity && capacity <= 20
            && status.compare(QStringLiteral("Discharging"),
                              Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}
}

K_PLUGIN_CLASS_WITH_JSON(FluffUpdates, "kcm_fluffupdates.json")

FluffUpdates::FluffUpdates(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
{
    // KCMs are loaded inside System Settings rather than as their own
    // application. Register this KCM's installed catalog directory
    // explicitly so its domain is found regardless of the host process's
    // translation domain or launch method.
    KLocalizedString::addDomainLocaleDir(
        QByteArrayLiteral("kcm_fluffupdates"),
        QStringLiteral(FLUFFLINUX_LOCALE_DIR));

    // Plasma can keep its preferred interface languages separately from the
    // process locale. This is common when the base session remains C.UTF-8.
    // Honour the same ordered language list used by the Plasma workspace.
    const QString plasmaLocalePath =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/plasma-localerc");
    QSettings plasmaLocale(plasmaLocalePath, QSettings::IniFormat);
    plasmaLocale.beginGroup(QStringLiteral("Translations"));
    const QString configuredLanguages =
        plasmaLocale.value(QStringLiteral("LANGUAGE")).toString();
    plasmaLocale.endGroup();
    if (!configuredLanguages.trimmed().isEmpty()) {
        QStringList languages =
            configuredLanguages.split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (QString &language : languages) {
            language = language.trimmed();
        }
        languages.removeAll(QString());
        if (!languages.isEmpty()) {
            KLocalizedString::setLanguages(languages);
        }
    }

    setButtons(NoAdditionalButton);

    // kcmshell6 wraps this QML page in a QWidget shell. Its internal
    // QQuickWindow is not necessarily the user-resizable top-level window, so
    // constrain both the Qt Quick window and the actual QWidget host.
    connect(this, &KQuickConfigModule::mainUiReady, this, [this] {
        QQuickItem *item = mainUi();
        if (!item) {
            return;
        }
        const QString windowTitle =
            i18nd("kcm_fluffupdates", "System Updates")
            + QStringLiteral(" — ")
            + i18nd("kcm_fluffupdates", "Fluff Linux Update");

        const auto applyQuickMinimum = [item, windowTitle] {
            if (QQuickWindow *window = item->window()) {
                const QSize current = window->minimumSize();
                window->setMinimumSize(
                    QSize(qMax(current.width(), 480),
                          qMax(current.height(), 400)));
                window->setTitle(windowTitle);
            }
        };
        applyQuickMinimum();
        connect(item, &QQuickItem::windowChanged, item,
                [applyQuickMinimum](QQuickWindow *) {
            applyQuickMinimum();
        });

        const auto applyWidgetMinimum = [this, windowTitle] {
            QWidget *host = nullptr;
            for (QObject *ancestor = this->parent(); ancestor;
                 ancestor = ancestor->parent()) {
                if (auto *widget = qobject_cast<QWidget *>(ancestor)) {
                    host = widget->window();
                }
            }
            if (!host) {
                host = QApplication::activeWindow();
            }
            if (!host) {
                return;
            }
            const QSize current = host->minimumSize();
            host->setMinimumSize(
                QSize(qMax(current.width(), 480),
                      qMax(current.height(), 400)));
            host->setWindowTitle(windowTitle);
        };
        applyWidgetMinimum();
        QTimer::singleShot(0, this, applyWidgetMinimum);
        QTimer::singleShot(250, this, applyWidgetMinimum);
    });

    if (QNetworkInformation::loadDefaultBackend()) {
        m_networkInformation = QNetworkInformation::instance();
        connect(m_networkInformation, &QNetworkInformation::reachabilityChanged,
                this, [this] { updateNetworkState(); });
    }
    updateNetworkState();

    m_stateWatcher = new QFileSystemWatcher(this);
    m_stateWatcher->addPath(QStringLiteral("/etc/pacman.d"));
    if (QFileInfo::exists(QString::fromLatin1(StateFile))) {
        m_stateWatcher->addPath(QString::fromLatin1(StateFile));
    }
    if (QFileInfo::exists(QString::fromLatin1(InstallStateFile))) {
        m_stateWatcher->addPath(QString::fromLatin1(InstallStateFile));
    }
    connect(m_stateWatcher, &QFileSystemWatcher::fileChanged, this, [this] {
        readStateFile();
        readInstallState();
    });
    connect(m_stateWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        readStateFile();
        readInstallState();
    });

    readStateFile();
    // Completed/cancelled state belongs to the previous panel session. Only
    // restore an active worker when the KCM is opened again.
    m_ignoreInactiveInstallState = true;
    readInstallState();

    // QFileSystemWatcher remains the immediate update path. Poll only while a
    // transaction is active as a fallback for coalesced events and QSaveFile
    // replacing the watched inode.
    auto *installPollTimer = new QTimer(this);
    installPollTimer->setInterval(750);
    connect(installPollTimer, &QTimer::timeout, this, [this] {
        if (updateActive()) {
            readInstallState();
        }
    });
    installPollTimer->start();

    // Battery capacity/status files are sysfs attributes and are not reliable
    // QFileSystemWatcher targets. Poll only from an available-update result
    // through installation, so power changes remain live without permanent
    // polling.
    auto *batteryTimer = new QTimer(this);
    batteryTimer->setInterval(2000);
    connect(batteryTimer, &QTimer::timeout,
            this, &FluffUpdates::updateBatteryState);
    connect(this, &FluffUpdates::checkStateChanged, this,
            [this, batteryTimer] {
        const bool batteryMonitoringRelevant =
            m_checkComplete && !m_checking && m_checkError.isEmpty()
            && m_updatesAvailable;
        updateBatteryState();
        if (batteryMonitoringRelevant && !batteryTimer->isActive()) {
            batteryTimer->start();
        } else if (!batteryMonitoringRelevant) {
            batteryTimer->stop();
        }
    });
}

QString FluffUpdates::lastUpdate() const
{
    return m_lastUpdate;
}

bool FluffUpdates::hasLastUpdate() const
{
    return !m_lastUpdate.isEmpty();
}

QString FluffUpdates::stateMessage() const
{
    return m_stateMessage;
}

QString FluffUpdates::freshnessText() const
{
    return m_freshnessText;
}

QString FluffUpdates::freshnessColor() const
{
    return m_freshnessColor;
}

QString FluffUpdates::relativeTime() const
{
    return m_relativeTime;
}

bool FluffUpdates::checking() const { return m_checking; }
bool FluffUpdates::checkComplete() const { return m_checkComplete; }
bool FluffUpdates::updatesAvailable() const { return m_updatesAvailable; }
QString FluffUpdates::downloadSize() const { return m_downloadSize; }
QString FluffUpdates::diskChange() const { return m_diskChange; }
bool FluffUpdates::diskSpaceFreed() const { return m_diskSpaceFreed; }
QString FluffUpdates::checkError() const { return m_checkError; }
QVariantList FluffUpdates::updatePackages() const { return m_updatePackages; }
bool FluffUpdates::batteryLow() const { return m_batteryLow; }
QString FluffUpdates::installPhase() const { return m_installPhase; }
bool FluffUpdates::updateActive() const
{
    return m_installPhase == QStringLiteral("starting")
        || m_installPhase == QStringLiteral("downloading")
        || m_installPhase == QStringLiteral("installing");
}
double FluffUpdates::installProgress() const { return m_installProgress; }
int FluffUpdates::completedPackages() const { return m_completedPackages; }
int FluffUpdates::totalPackages() const { return m_totalPackages; }
QString FluffUpdates::downloadedSize() const
{
    return humanDataSize(m_downloadedBytes);
}
QString FluffUpdates::totalDownloadSize() const
{
    return humanDataSize(m_totalDownloadBytes);
}
QString FluffUpdates::downloadSpeed() const { return m_downloadSpeed; }
QString FluffUpdates::installError() const { return m_installError; }
bool FluffUpdates::cancellationNotice() const { return m_cancellationNotice; }
bool FluffUpdates::installationSuccessNotice() const
{
    return m_installationSuccessNotice;
}
bool FluffUpdates::networkConnected() const { return m_networkConnected; }
bool FluffUpdates::networkLimited() const { return m_networkLimited; }

void FluffUpdates::updateNetworkState()
{
    bool connected = false;
    bool limited = false;
    if (m_networkInformation
        && m_networkInformation->reachability() != QNetworkInformation::Reachability::Unknown) {
        const auto reachability = m_networkInformation->reachability();
        connected = reachability != QNetworkInformation::Reachability::Disconnected;
        limited = reachability == QNetworkInformation::Reachability::Local
            || reachability == QNetworkInformation::Reachability::Site;
    } else {
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &interface : interfaces) {
            const auto flags = interface.flags();
            if (flags.testFlag(QNetworkInterface::IsUp)
                && flags.testFlag(QNetworkInterface::IsRunning)
                && !flags.testFlag(QNetworkInterface::IsLoopBack)) {
                connected = true;
                break;
            }
        }
    }

    if (m_networkConnected != connected) {
        m_networkConnected = connected;
        Q_EMIT networkConnectedChanged();
    }
    if (m_networkLimited != limited) {
        m_networkLimited = limited;
        Q_EMIT networkLimitedChanged();
    }
}

void FluffUpdates::updateBatteryState()
{
    const bool batteryMonitoringRelevant =
        m_checkComplete && !m_checking && m_checkError.isEmpty()
        && m_updatesAvailable;
    const bool batteryLow =
        batteryMonitoringRelevant && systemBatteryIsLow();

    if (m_batteryLow != batteryLow) {
        m_batteryLow = batteryLow;
        Q_EMIT batteryStateChanged();
    }
}

void FluffUpdates::refresh()
{
    readStateFile();
}

void FluffUpdates::checkForUpdates()
{
    if (m_checking || !m_networkConnected) {
        return;
    }

    m_ignoreInactiveInstallState = true;
    m_installPhase = QStringLiteral("idle");
    m_installError.clear();
    if (pacmanRunning()) {
        m_checkComplete = true;
        m_updatesAvailable = false;
        m_checkError = i18nd("kcm_fluffupdates", "pacman process is already running.");
        Q_EMIT checkStateChanged();
        Q_EMIT installStateChanged();
        return;
    }

    m_checking = true;
    m_checkStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_checkComplete = false;
    m_updatesAvailable = false;
    m_diskSpaceFreed = false;
    m_downloadSize.clear();
    m_diskChange.clear();
    m_checkError.clear();
    if (!m_updatePackages.isEmpty()) {
        m_updatePackages.clear();
        Q_EMIT updatePackagesChanged();
    }
    Q_EMIT checkStateChanged();
    Q_EMIT installStateChanged();

    m_checkDatabasePath = QStringLiteral("/tmp/flufflinux-checkupdates-%1").arg(geteuid());
    m_checkProcess = new QProcess(this);
    m_checkProcess->setProgram(QStringLiteral("checkupdates"));
    m_checkProcess->setArguments({QStringLiteral("--nocolor")});
    m_checkProcess->setProcessChannelMode(QProcess::MergedChannels);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CHECKUPDATES_DB"), m_checkDatabasePath);
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    m_checkProcess->setProcessEnvironment(environment);

    connect(m_checkProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            afterMinimumCheckDuration([this] {
                m_checking = false;
                m_checkComplete = true;
                m_checkError = i18nd(
                    "kcm_fluffupdates",
                    "The update checker could not be started. Make sure pacman-contrib is installed.");
                Q_EMIT checkStateChanged();
            });
        }
    });
    connect(m_checkProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(m_checkProcess->readAll()).trimmed();
        m_checkProcess->deleteLater();
        m_checkProcess = nullptr;

        if (exitStatus != QProcess::NormalExit) {
            afterMinimumCheckDuration([this] {
                m_checking = false;
                m_checkComplete = true;
                m_checkError = i18nd(
                    "kcm_fluffupdates",
                    "The update check stopped unexpectedly.");
                Q_EMIT checkStateChanged();
            });
        } else if (exitCode == 2 && !output.contains(QStringLiteral("error:"), Qt::CaseInsensitive)) {
            afterMinimumCheckDuration([this] {
                m_updatesAvailable = false;
                if (!hasLastUpdate()) {
                    recordInitialUpdate();
                } else {
                    m_checking = false;
                    m_checkComplete = true;
                    Q_EMIT checkStateChanged();
                }
            });
        } else if (exitCode != 0) {
            afterMinimumCheckDuration([this, output] {
                m_checking = false;
                m_checkComplete = true;
                m_checkError = friendlyCheckError(output);
                Q_EMIT checkStateChanged();
            });
        } else {
            m_updatesAvailable = true;
            m_updatePackages = parseUpdatePackages(output);
            Q_EMIT updatePackagesChanged();
            readTransactionSummary();
        }
    });

    m_checkProcess->start();
}

void FluffUpdates::afterMinimumCheckDuration(
    std::function<void()> completion)
{
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - m_checkStartedAtMs;
    const int remaining =
        static_cast<int>(qMax<qint64>(0, 2000 - elapsed));
    QTimer::singleShot(remaining, this,
                       [completion = std::move(completion)]() mutable {
        completion();
    });
}

void FluffUpdates::recordInitialUpdate()
{
    m_summaryProcess = new QProcess(this);
    m_summaryProcess->setProgram(QStringLiteral("pkexec"));
    m_summaryProcess->setArguments({
        QStringLiteral("/usr/lib/flufflinux-update/flufflinux-update-helper"),
        QStringLiteral("--record-current-update"),
    });
    m_summaryProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_summaryProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_summaryProcess) {
            return;
        }
        m_summaryProcess->deleteLater();
        m_summaryProcess = nullptr;
        m_checking = false;
        m_checkComplete = true;
        m_checkError = i18nd(
            "kcm_fluffupdates", "The privileged update check could not be started.");
        Q_EMIT checkStateChanged();
    });
    connect(m_summaryProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_summaryProcess) {
            return;
        }
        const QString output =
            QString::fromLocal8Bit(m_summaryProcess->readAll());
        m_summaryProcess->deleteLater();
        m_summaryProcess = nullptr;

        m_checking = false;
        m_checkComplete = true;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString lower = output.toLower();
            if (exitCode == 126 || exitCode == 127
                || lower.contains(QStringLiteral("not authorized"))
                || lower.contains(QStringLiteral("not allowed"))
                || lower.contains(QStringLiteral("authentication failed"))
                || lower.contains(QStringLiteral("dismissed"))) {
                m_checkError = i18nd(
                    "kcm_fluffupdates", "Authorization error, please try again.");
            } else {
                m_checkError = i18nd(
                    "kcm_fluffupdates",
                    "The privileged update check could not be started.");
            }
        } else {
            readStateFile();
        }
        Q_EMIT checkStateChanged();
    });
    m_summaryProcess->start();
}

void FluffUpdates::clearCheckResult()
{
    if (m_checking || updateActive() || !m_checkComplete) {
        return;
    }

    m_checkComplete = false;
    m_updatesAvailable = false;
    m_diskSpaceFreed = false;
    m_downloadSize.clear();
    m_diskChange.clear();
    m_checkError.clear();
    if (!m_updatePackages.isEmpty()) {
        m_updatePackages.clear();
        Q_EMIT updatePackagesChanged();
    }
    Q_EMIT checkStateChanged();
}

void FluffUpdates::startInstallation()
{
    if (updateActive() || !m_updatesAvailable || m_checkError.length() > 0
        || !m_networkConnected) {
        return;
    }

    m_installPhase = QStringLiteral("starting");
    m_ignoreInactiveInstallState = false;
    m_installError.clear();
    m_installProgress = 0;
    Q_EMIT installStateChanged();

    m_installControlProcess = new QProcess(this);
    m_installControlProcess->setProgram(QStringLiteral("pkexec"));
    m_installControlProcess->setArguments({
        QStringLiteral("/usr/lib/flufflinux-update/flufflinux-update-helper"),
        QStringLiteral("--install"),
        m_downloadSize,
        m_diskChange,
        m_diskSpaceFreed ? QStringLiteral("true") : QStringLiteral("false"),
        QString::fromLatin1(
            QJsonDocument(QJsonArray::fromVariantList(m_updatePackages))
                .toJson(QJsonDocument::Compact)
                .toBase64(QByteArray::Base64UrlEncoding
                          | QByteArray::OmitTrailingEquals)),
    });
    m_installControlProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_installControlProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_installControlProcess) {
            m_installControlProcess->deleteLater();
            m_installControlProcess = nullptr;
            m_installPhase = QStringLiteral("failed");
            m_installError = i18nd("kcm_fluffupdates",
                                   "The update process could not be started.");
            Q_EMIT installStateChanged();
        }
    });
    connect(m_installControlProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_installControlProcess) {
            return;
        }
        const QString output =
            QString::fromLocal8Bit(m_installControlProcess->readAll());
        m_installControlProcess->deleteLater();
        m_installControlProcess = nullptr;

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_installPhase = QStringLiteral("failed");
            if (output.contains(QStringLiteral("PACMAN_RUNNING"))) {
                m_installError =
                    i18nd("kcm_fluffupdates", "pacman process is already running.");
            } else if (exitCode == 126 || exitCode == 127
                       || output.contains(QStringLiteral("not authorized"),
                                          Qt::CaseInsensitive)
                       || output.contains(QStringLiteral("dismissed"),
                                          Qt::CaseInsensitive)) {
                m_installError = i18nd(
                    "kcm_fluffupdates", "Authorization error, please try again.");
            } else {
                m_installError = i18nd(
                    "kcm_fluffupdates", "The update process could not be started.");
            }
            Q_EMIT installStateChanged();
        }
    });
    m_installControlProcess->start();
}

void FluffUpdates::cancelInstallation()
{
    if (m_installPhase != QStringLiteral("downloading")
        || m_installControlProcess) {
        return;
    }

    m_installControlProcess = new QProcess(this);
    m_installControlProcess->setProgram(QStringLiteral("pkexec"));
    m_installControlProcess->setArguments({
        QStringLiteral("/usr/lib/flufflinux-update/flufflinux-update-helper"),
        QStringLiteral("--cancel"),
    });
    m_installControlProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_installControlProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_installControlProcess) {
            return;
        }
        const QString output =
            QString::fromLocal8Bit(m_installControlProcess->readAll());
        m_installControlProcess->deleteLater();
        m_installControlProcess = nullptr;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            m_installError = (exitCode == 126 || exitCode == 127
                              || output.contains(QStringLiteral("dismissed"),
                                                 Qt::CaseInsensitive))
                ? i18nd("kcm_fluffupdates",
                        "Authorization error, please try again.")
                : i18nd("kcm_fluffupdates",
                        "The update process could not be cancelled.");
            Q_EMIT installStateChanged();
        }
    });
    m_installControlProcess->start();
}

void FluffUpdates::readTransactionSummary()
{
    m_summaryProcess = new QProcess(this);
    m_summaryProcess->setProgram(QStringLiteral("pkexec"));
    m_summaryProcess->setArguments({
        QStringLiteral("/usr/lib/flufflinux-update/flufflinux-update-helper"),
        m_checkDatabasePath,
    });
    m_summaryProcess->setProcessChannelMode(QProcess::MergedChannels);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    m_summaryProcess->setProcessEnvironment(environment);

    connect(m_summaryProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_summaryProcess) {
            m_summaryProcess->deleteLater();
            m_summaryProcess = nullptr;
            afterMinimumCheckDuration([this] {
                m_checking = false;
                m_checkComplete = true;
                m_checkError = i18nd(
                    "kcm_fluffupdates",
                    "The privileged update check could not be started.");
                Q_EMIT checkStateChanged();
            });
        }
    });
    connect(m_summaryProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_summaryProcess) {
            return;
        }
        QString output = QString::fromLocal8Bit(m_summaryProcess->readAll());
        m_summaryProcess->deleteLater();
        m_summaryProcess = nullptr;

        // A user's pacman.conf may force colored output even when stdout is a
        // pipe. Remove ANSI terminal sequences before matching the summary.
        output.remove(QRegularExpression(QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]")));

        const QString lowerOutput = output.toLower();
        if (exitStatus != QProcess::NormalExit) {
            m_checkError = i18nd("kcm_fluffupdates", "The privileged update check stopped unexpectedly.");
        } else if (lowerOutput.contains(QStringLiteral("not authorized"))
                   || lowerOutput.contains(QStringLiteral("not allowed"))
                   || lowerOutput.contains(QStringLiteral("authentication failed"))) {
            m_checkError = i18nd("kcm_fluffupdates", "Authorization error, please try again.");
        } else if (exitCode == 126
                   || lowerOutput.contains(QStringLiteral("dismissed"))) {
            m_checkError = i18nd("kcm_fluffupdates", "Authorization error, please try again.");
        } else if (exitCode == 127) {
            m_checkError = i18nd("kcm_fluffupdates", "Authorization error, please try again.");
        }

        const QRegularExpression downloadExpression(
            QStringLiteral("Total Download Size:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([^\\s]+)"));
        const QRegularExpression installedExpression(
            QStringLiteral("Total Installed Size:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([^\\s]+)"));
        const QRegularExpression netExpression(
            QStringLiteral("Net Upgrade Size:\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*([^\\s]+)"));
        const auto downloadMatch = downloadExpression.match(output);
        const auto installedMatch = installedExpression.match(output);
        const auto netMatch = netExpression.match(output);

        if (!m_checkError.isEmpty()) {
            // Keep the authentication error selected above.
        } else if (downloadMatch.hasMatch()) {
            m_downloadSize = downloadMatch.captured(1) + QLatin1Char(' ') + downloadMatch.captured(2);
        } else if (installedMatch.hasMatch()) {
            // Pacman omits Total Download Size when all archives are cached.
            m_downloadSize = i18nd("kcm_fluffupdates", "No additional download required");
        } else {
            m_checkError = i18nd("kcm_fluffupdates", "Update sizes could not be calculated.");
        }

        if (!m_checkError.isEmpty()) {
            // Do not replace the authentication error with a parsing error.
        } else if (netMatch.hasMatch()) {
            const double netValue = netMatch.captured(1).toDouble();
            m_diskSpaceFreed = netValue < 0.0;
            m_diskChange = QString::number(qAbs(netValue), 'f', 2)
                + QLatin1Char(' ') + netMatch.captured(2);
        } else {
            m_checkError = i18nd("kcm_fluffupdates", "Update sizes could not be calculated.");
        }

        afterMinimumCheckDuration([this] {
            m_checking = false;
            m_checkComplete = true;
            Q_EMIT checkStateChanged();
        });
    });
    m_summaryProcess->start();
}

void FluffUpdates::readInstallState()
{
    QFile file(QString::fromLatin1(InstallStateFile));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return;
    }

    const QJsonObject state = document.object();
    const QString phase = state.value(QStringLiteral("phase")).toString();
    if (phase.isEmpty()) {
        return;
    }
    const bool activeState = phase == QStringLiteral("starting")
        || phase == QStringLiteral("downloading")
        || phase == QStringLiteral("installing");
    if (m_ignoreInactiveInstallState && !activeState) {
        return;
    }
    if (activeState) {
        // Once we reconnect to a live worker, accept its eventual completion
        // or failure as part of this panel session.
        m_ignoreInactiveInstallState = false;
    }
    const QString previousPhase = m_installPhase;
    m_installPhase = phase;
    m_installProgress = state.value(QStringLiteral("progress")).toDouble();
    m_completedPackages =
        state.value(QStringLiteral("completed_packages")).toInt();
    m_totalPackages = state.value(QStringLiteral("total_packages")).toInt();
    m_downloadedBytes =
        state.value(QStringLiteral("downloaded_bytes")).toInteger();
    m_totalDownloadBytes =
        state.value(QStringLiteral("total_download_bytes")).toInteger();
    m_downloadSpeed = state.value(QStringLiteral("speed")).toString();

    const QVariantList savedPackages =
        state.value(QStringLiteral("updates")).toArray().toVariantList();
    if (!savedPackages.isEmpty() && savedPackages != m_updatePackages) {
        m_updatePackages = savedPackages;
        Q_EMIT updatePackagesChanged();
    }

    const QString savedDownload =
        state.value(QStringLiteral("download_size")).toString();
    const QString savedStorage =
        state.value(QStringLiteral("storage_change")).toString();
    if (!savedDownload.isEmpty()) {
        m_downloadSize = savedDownload;
    }
    if (!savedStorage.isEmpty()) {
        m_diskChange = savedStorage;
        m_diskSpaceFreed =
            state.value(QStringLiteral("storage_freed")).toBool();
    }

    m_installError.clear();
    if (phase == QStringLiteral("failed")) {
        const QString error = state.value(QStringLiteral("error")).toString();
        if (error == QStringLiteral("DOWNLOAD_FAILED")
            || error == QStringLiteral("TRANSACTION_PREPARE_FAILED")) {
            m_installError = i18nd(
                "kcm_fluffupdates",
                "The update download failed. Check your connection and try again.");
        } else if (error == QStringLiteral("INSTALL_FAILED")) {
            m_installError =
                i18nd("kcm_fluffupdates", "The system update failed.");
        } else {
            m_installError =
                i18nd("kcm_fluffupdates", "The update process could not be started.");
        }
    } else if (phase == QStringLiteral("cancelled")) {
        m_checkComplete = true;
        m_updatesAvailable = true;
        if (previousPhase != phase) {
            m_cancellationNotice = true;
            QTimer::singleShot(5000, this, [this] {
                m_cancellationNotice = false;
                Q_EMIT installStateChanged();
            });
        }
    } else if (phase == QStringLiteral("complete")) {
        m_checkComplete = true;
        m_updatesAvailable = false;
        const bool completedActiveInstallation =
            previousPhase == QStringLiteral("starting")
            || previousPhase == QStringLiteral("downloading")
            || previousPhase == QStringLiteral("installing");
        if (completedActiveInstallation) {
            m_installationSuccessNotice = true;
            QTimer::singleShot(7000, this, [this] {
                m_installationSuccessNotice = false;
                Q_EMIT installStateChanged();
            });
        }
        readStateFile();
        Q_EMIT checkStateChanged();
    } else if (updateActive()) {
        m_checkComplete = true;
        m_updatesAvailable = true;
    }

    const QString statePath = QString::fromLatin1(InstallStateFile);
    if (QFileInfo::exists(statePath)
        && !m_stateWatcher->files().contains(statePath)) {
        m_stateWatcher->addPath(statePath);
    }
    Q_EMIT installStateChanged();
    Q_EMIT checkStateChanged();
}

void FluffUpdates::readStateFile()
{
    QString lastUpdate;
    QString message;
    QString freshnessText;
    QString freshnessColor = QStringLiteral("#3daee9");
    QString relativeTime;
    QFile file(QString::fromLatin1(StateFile));

    if (!file.exists()) {
        message = i18nd("kcm_fluffupdates", "System was not previously updated.");
    } else if (!file.open(QIODevice::ReadOnly)) {
        message = i18nd("kcm_fluffupdates", "The last update information could not be read.");
    } else {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            message = i18nd("kcm_fluffupdates", "The last update information is invalid.");
        } else {
            const QString storedUpdate = document.object()
                .value(QString::fromLatin1(LastUpdateKey)).toString().trimmed();
            if (storedUpdate.isEmpty()) {
                message = i18nd("kcm_fluffupdates", "System was not previously updated.");
            } else {
                lastUpdate = storedUpdate;
            }
        }
    }

    if (lastUpdate.isEmpty()) {
        freshnessText = i18nd("kcm_fluffupdates", "Updates were not installed on this system");
    } else {
        const QDateTime recorded = parseLastUpdate(lastUpdate);
        const QDateTime now = QDateTime::currentDateTime();
        if (!recorded.isValid()) {
            freshnessText = i18nd("kcm_fluffupdates", "Update date available");
        } else if (recorded < now.addMonths(-3)) {
            freshnessText = i18nd("kcm_fluffupdates", "Last update was installed more than three months ago");
            freshnessColor = QStringLiteral("#f2994a");
        } else if (recorded < now.addMonths(-1)) {
            freshnessText = i18nd("kcm_fluffupdates", "Last update was installed more than a month ago");
            freshnessColor = QStringLiteral("#f2c94c");
        } else {
            freshnessText = i18nd("kcm_fluffupdates", "Updated recently");
            freshnessColor = QStringLiteral("#27ae60");
        }

        if (recorded.isValid()) {
            const qint64 seconds = qMax<qint64>(0, recorded.secsTo(now));
            if (seconds < 60) {
                relativeTime = i18ndp("kcm_fluffupdates", "%1 second ago", "%1 seconds ago", seconds);
            } else if (seconds < 3600) {
                const qint64 minutes = seconds / 60;
                relativeTime = i18ndp("kcm_fluffupdates", "%1 minute ago", "%1 minutes ago", minutes);
            } else if (seconds < 86400) {
                const qint64 hours = seconds / 3600;
                relativeTime = i18ndp("kcm_fluffupdates", "%1 hour ago", "%1 hours ago", hours);
            } else {
                const qint64 days = seconds / 86400;
                relativeTime = i18ndp("kcm_fluffupdates", "%1 day ago", "%1 days ago", days);
            }
        }
    }

    if (m_lastUpdate == lastUpdate && m_stateMessage == message
        && m_freshnessText == freshnessText && m_freshnessColor == freshnessColor
        && m_relativeTime == relativeTime) {
        return;
    }

    m_lastUpdate = lastUpdate;
    m_stateMessage = message;
    m_freshnessText = freshnessText;
    m_freshnessColor = freshnessColor;
    m_relativeTime = relativeTime;

    const QString statePath = QString::fromLatin1(StateFile);
    if (QFileInfo::exists(statePath) && !m_stateWatcher->files().contains(statePath)) {
        m_stateWatcher->addPath(statePath);
    }
    Q_EMIT lastUpdateChanged();
}

#include "fluffupdates.moc"
