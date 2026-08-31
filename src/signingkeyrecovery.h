#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

struct SigningKeyCommandResult
{
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray output;
};

struct SigningKeyRecoveryResult
{
    bool recovered = false;
    QString repository;
    QString category;
    QString expectedFingerprint;
    QString receivedFingerprint;
    QString requestedFingerprint;
};

struct SigningKeyRecoveryConfig
{
    QString gpgPath = QStringLiteral("/usr/bin/gpg");
    QString pacmanKeyPath = QStringLiteral("/usr/bin/pacman-key");
    int maximumFingerprintResponseBytes = 4096;
    int maximumCertificateResponseBytes = 256 * 1024;
};

class SigningKeyRecovery
{
public:
    using Fetcher = std::function<QByteArray(QString *failureCategory)>;
    using Runner = std::function<SigningKeyCommandResult(
        const QString &program, const QStringList &arguments)>;

    SigningKeyRecovery(SigningKeyRecoveryConfig config,
                       Fetcher fingerprintFetcher,
                       Fetcher certificateFetcher, Runner runner);

    SigningKeyRecoveryResult recover(const QString &pacmanOutput) const;

    static QString normalizeFingerprint(const QByteArray &value);
    static QString requestedFingerprint(const QString &pacmanOutput);
    static QString repositoryName(const QString &pacmanOutput);
    static bool containsUnknownKeyReport(const QString &pacmanOutput);

private:
    SigningKeyCommandResult run(const QString &program,
                                const QStringList &arguments) const;

    SigningKeyRecoveryConfig m_config;
    Fetcher m_fingerprintFetcher;
    Fetcher m_certificateFetcher;
    Runner m_runner;
};
