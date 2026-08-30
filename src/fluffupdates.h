#pragma once

#include <KQuickConfigModule>

#include <QString>
#include <QStringList>
#include <QSet>
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
    Q_PROPERTY(bool signingKeySecurityError READ signingKeySecurityError NOTIFY installStateChanged)
    Q_PROPERTY(QString signingKeyTechnicalDetails READ signingKeyTechnicalDetails NOTIFY installStateChanged)
    Q_PROPERTY(QString signingKeyIssueUrl READ signingKeyIssueUrl NOTIFY installStateChanged)
    Q_PROPERTY(bool cancellationNotice READ cancellationNotice NOTIFY installStateChanged)
    Q_PROPERTY(bool installationSuccessNotice READ installationSuccessNotice NOTIFY installStateChanged)
    Q_PROPERTY(bool pacmanView READ pacmanView WRITE setPacmanView NOTIFY pacmanViewChanged)
    Q_PROPERTY(int updateWindowWidth READ updateWindowWidth CONSTANT)
    Q_PROPERTY(int updateWindowHeight READ updateWindowHeight CONSTANT)
    Q_PROPERTY(bool updateWindowMaximized READ updateWindowMaximized CONSTANT)
    Q_PROPERTY(bool networkConnected READ networkConnected NOTIFY networkConnectedChanged)
    Q_PROPERTY(bool networkLimited READ networkLimited NOTIFY networkLimitedChanged)
    Q_PROPERTY(QString recoveryDialogType READ recoveryDialogType NOTIFY recoveryDialogChanged)
    Q_PROPERTY(QString recoveryPackage READ recoveryPackage NOTIFY recoveryDialogChanged)
    Q_PROPERTY(QString recoveryNotice READ recoveryNotice NOTIFY recoveryNoticeChanged)
    Q_PROPERTY(QString recoveryActionState READ recoveryActionState NOTIFY recoveryDialogChanged)

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
    bool signingKeySecurityError() const;
    QString signingKeyTechnicalDetails() const;
    QString signingKeyIssueUrl() const;
    bool cancellationNotice() const;
    bool installationSuccessNotice() const;
    bool pacmanView() const;
    int updateWindowWidth() const;
    int updateWindowHeight() const;
    bool updateWindowMaximized() const;
    bool networkConnected() const;
    bool networkLimited() const;
    QString recoveryDialogType() const;
    QString recoveryPackage() const;
    QString recoveryNotice() const;
    QString recoveryActionState() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void clearCheckResult();
    Q_INVOKABLE void startInstallation();
    Q_INVOKABLE void cancelInstallation();
    Q_INVOKABLE void retrySigningKeyUpdate();
    Q_INVOKABLE void copySigningKeyTechnicalDetails();
    Q_INVOKABLE void openSigningKeyIssue();
    Q_INVOKABLE void setPacmanView(bool enabled);
    Q_INVOKABLE void saveUpdateWindowState(int width, int height,
                                           bool maximized);
    Q_INVOKABLE void resolveRemovalWarning(bool allowRemoval);

Q_SIGNALS:
    void lastUpdateChanged();
    void checkStateChanged();
    void installStateChanged();
    void networkConnectedChanged();
    void networkLimitedChanged();
    void batteryStateChanged();
    void updatePackagesChanged();
    void pacmanViewChanged();
    void recoveryDialogChanged();
    void recoveryNoticeChanged();

private:
    void readStateFile();
    void startUpdateQuery();
    void readTransactionSummary();
    void recordInitialUpdate();
    void afterMinimumCheckDuration(std::function<void()> completion);
    void readInstallState();
    void updateNetworkState();
    void updateBatteryState();
    void removeBlockingPackage(const QString &package);
    void recoverCheckSigningKey(const QString &output, int pacmanExitStatus);
    void showCheckSigningKeyFailure(const QString &category,
                                    const QString &repository,
                                    const QString &expected,
                                    const QString &received,
                                    const QString &requested,
                                    int pacmanExitStatus);
    void restartUpdateCheck();
    void showSigningKeyVerifiedNotice();
    void showPendingAutoremoveNotice();

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
    bool m_checkSigningKeyRecoveryAttempted = false;
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
    bool m_signingKeySecurityError = false;
    QString m_signingKeyRepository;
    QString m_signingKeyExpectedFingerprint;
    QString m_signingKeyReceivedFingerprint;
    QString m_signingKeyRequestedFingerprint;
    QString m_signingKeyFailureCategory;
    int m_signingKeyPacmanExitStatus = -1;
    bool m_cancellationNotice = false;
    bool m_installationSuccessNotice = false;
    quint64 m_installationSuccessNoticeGeneration = 0;
    bool m_ignoreInactiveInstallState = false;
    bool m_pacmanView = false;
    int m_updateWindowWidth = 0;
    int m_updateWindowHeight = 0;
    bool m_updateWindowMaximized = false;
    QNetworkInformation *m_networkInformation = nullptr;
    bool m_networkConnected = true;
    bool m_networkLimited = false;
    bool m_batteryLow = false;
    QString m_recoveryDialogType;
    QString m_recoveryPackage;
    QString m_recoveryNotice;
    QString m_recoveryActionState;
    quint64 m_recoveryNoticeGeneration = 0;
    bool m_recoveryRequiresImmediateRemoval = false;
    bool m_recoveryRestartPending = false;
    QStringList m_pendingAutoremovedPackages;
    QSet<QString> m_approvedRemovals;
    qint64 m_lastRecoveryNoticeId = 0;
};
