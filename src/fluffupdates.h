#pragma once

#include <KQuickConfigModule>

#include <QString>
#include <QVariantList>

#include <functional>

class QFileSystemWatcher;
class QProcess;
class QNetworkInformation;

class FluffUpdates final : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(QString lastUpdate READ lastUpdate NOTIFY lastUpdateChanged)
    Q_PROPERTY(bool hasLastUpdate READ hasLastUpdate NOTIFY lastUpdateChanged)
    Q_PROPERTY(QString stateMessage READ stateMessage NOTIFY lastUpdateChanged)
    Q_PROPERTY(QString freshnessText READ freshnessText NOTIFY lastUpdateChanged)
    Q_PROPERTY(QString freshnessColor READ freshnessColor NOTIFY lastUpdateChanged)
    Q_PROPERTY(QString relativeTime READ relativeTime NOTIFY lastUpdateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkStateChanged)
    Q_PROPERTY(bool checkComplete READ checkComplete NOTIFY checkStateChanged)
    Q_PROPERTY(bool updatesAvailable READ updatesAvailable NOTIFY checkStateChanged)
    Q_PROPERTY(QString downloadSize READ downloadSize NOTIFY checkStateChanged)
    Q_PROPERTY(QString diskChange READ diskChange NOTIFY checkStateChanged)
    Q_PROPERTY(bool diskSpaceFreed READ diskSpaceFreed NOTIFY checkStateChanged)
    Q_PROPERTY(QString checkError READ checkError NOTIFY checkStateChanged)
    Q_PROPERTY(QVariantList updatePackages READ updatePackages NOTIFY updatePackagesChanged)
    Q_PROPERTY(bool batteryLow READ batteryLow NOTIFY batteryStateChanged)
    Q_PROPERTY(QString installPhase READ installPhase NOTIFY installStateChanged)
    Q_PROPERTY(bool updateActive READ updateActive NOTIFY installStateChanged)
    Q_PROPERTY(double installProgress READ installProgress NOTIFY installStateChanged)
    Q_PROPERTY(int completedPackages READ completedPackages NOTIFY installStateChanged)
    Q_PROPERTY(int totalPackages READ totalPackages NOTIFY installStateChanged)
    Q_PROPERTY(QString downloadedSize READ downloadedSize NOTIFY installStateChanged)
    Q_PROPERTY(QString totalDownloadSize READ totalDownloadSize NOTIFY installStateChanged)
    Q_PROPERTY(QString downloadSpeed READ downloadSpeed NOTIFY installStateChanged)
    Q_PROPERTY(QString installError READ installError NOTIFY installStateChanged)
    Q_PROPERTY(bool cancellationNotice READ cancellationNotice NOTIFY installStateChanged)
    Q_PROPERTY(bool installationSuccessNotice READ installationSuccessNotice NOTIFY installStateChanged)
    Q_PROPERTY(bool networkConnected READ networkConnected NOTIFY networkConnectedChanged)
    Q_PROPERTY(bool networkLimited READ networkLimited NOTIFY networkLimitedChanged)

public:
    explicit FluffUpdates(QObject *parent, const KPluginMetaData &data);

    QString lastUpdate() const;
    bool hasLastUpdate() const;
    QString stateMessage() const;
    QString freshnessText() const;
    QString freshnessColor() const;
    QString relativeTime() const;
    bool checking() const;
    bool checkComplete() const;
    bool updatesAvailable() const;
    QString downloadSize() const;
    QString diskChange() const;
    bool diskSpaceFreed() const;
    QString checkError() const;
    QVariantList updatePackages() const;
    bool batteryLow() const;
    QString installPhase() const;
    bool updateActive() const;
    double installProgress() const;
    int completedPackages() const;
    int totalPackages() const;
    QString downloadedSize() const;
    QString totalDownloadSize() const;
    QString downloadSpeed() const;
    QString installError() const;
    bool cancellationNotice() const;
    bool installationSuccessNotice() const;
    bool networkConnected() const;
    bool networkLimited() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void clearCheckResult();
    Q_INVOKABLE void startInstallation();
    Q_INVOKABLE void cancelInstallation();

Q_SIGNALS:
    void lastUpdateChanged();
    void checkStateChanged();
    void installStateChanged();
    void networkConnectedChanged();
    void networkLimitedChanged();
    void batteryStateChanged();
    void updatePackagesChanged();

private:
    void readStateFile();
    void readTransactionSummary();
    void recordInitialUpdate();
    void afterMinimumCheckDuration(std::function<void()> completion);
    void readInstallState();
    void updateNetworkState();
    void updateBatteryState();

    QString m_lastUpdate;
    QString m_stateMessage;
    QString m_freshnessText;
    QString m_freshnessColor;
    QString m_relativeTime;
    QFileSystemWatcher *m_stateWatcher = nullptr;
    QProcess *m_checkProcess = nullptr;
    QProcess *m_summaryProcess = nullptr;
    QProcess *m_installControlProcess = nullptr;
    QString m_checkDatabasePath;
    bool m_checking = false;
    qint64 m_checkStartedAtMs = 0;
    bool m_checkComplete = false;
    bool m_updatesAvailable = false;
    bool m_diskSpaceFreed = false;
    QString m_downloadSize;
    QString m_diskChange;
    QString m_checkError;
    QVariantList m_updatePackages;
    QString m_installPhase = QStringLiteral("idle");
    double m_installProgress = 0.0;
    int m_completedPackages = 0;
    int m_totalPackages = 0;
    qint64 m_downloadedBytes = 0;
    qint64 m_totalDownloadBytes = 0;
    QString m_downloadSpeed;
    QString m_installError;
    bool m_cancellationNotice = false;
    bool m_installationSuccessNotice = false;
    bool m_ignoreInactiveInstallState = false;
    QNetworkInformation *m_networkInformation = nullptr;
    bool m_networkConnected = true;
    bool m_networkLimited = false;
    bool m_batteryLow = false;
};
