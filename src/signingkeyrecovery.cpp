#include "signingkeyrecovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <utility>

namespace
{
bool commandSucceeded(const SigningKeyCommandResult &result)
{
    return result.started && result.finished && result.exitCode == 0;
}

QString normalizedRecordFingerprint(const QByteArray &value)
{
    const QString candidate = QString::fromLatin1(value).trimmed().toUpper();
    static const QRegularExpression fingerprint(QStringLiteral("^[0-9A-F]{40}$"));
    return fingerprint.match(candidate).hasMatch() ? candidate : QString{};
}

struct CertificateDescription
{
    QString primary;
    bool primaryUsable = false;
    QStringList allSubkeys;
    QStringList usableSigningSubkeys;
};

CertificateDescription describeCertificate(const QByteArray &listing)
{
    CertificateDescription description;
    QByteArray pendingRecord;
    bool pendingUsable = false;
    bool pendingSigningCapable = false;
    const QList<QByteArray> lines = listing.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.isEmpty()) {
            continue;
        }
        if (fields[0] == "pub" || fields[0] == "sub") {
            pendingRecord = fields[0];
            const QByteArray validity = fields.size() > 1 ? fields[1] : QByteArray{};
            pendingUsable = validity != "r" && validity != "e"
                && validity != "d" && validity != "i";
            const QByteArray capabilities =
                fields.size() > 11 ? fields[11].toLower() : QByteArray{};
            pendingSigningCapable = capabilities.contains('s');
            continue;
        }
        if (fields[0] != "fpr" || fields.size() <= 9 || pendingRecord.isEmpty()) {
            continue;
        }
        const QString fingerprint = normalizedRecordFingerprint(fields[9]);
        if (fingerprint.isEmpty()) {
            pendingRecord.clear();
            continue;
        }
        if (pendingRecord == "pub" && description.primary.isEmpty()) {
            description.primary = fingerprint;
            description.primaryUsable = pendingUsable;
        } else if (pendingRecord == "sub") {
            description.allSubkeys.append(fingerprint);
            if (pendingUsable && pendingSigningCapable) {
                description.usableSigningSubkeys.append(fingerprint);
            }
        }
        pendingRecord.clear();
    }
    return description;
}

bool hasValidSubkeyBinding(const QByteArray &checkedListing,
                           const QString &primaryFingerprint,
                           const QString &requestedSubkeyFingerprint)
{
    const QByteArray primaryKeyId = primaryFingerprint.right(16).toLatin1();
    QString currentSubkey;
    bool awaitingSubkeyFingerprint = false;
    const QList<QByteArray> lines = checkedListing.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.isEmpty()) {
            continue;
        }
        if (fields[0] == "sub") {
            awaitingSubkeyFingerprint = true;
            currentSubkey.clear();
            continue;
        }
        if (awaitingSubkeyFingerprint && fields[0] == "fpr"
            && fields.size() > 9) {
            currentSubkey = normalizedRecordFingerprint(fields[9]);
            awaitingSubkeyFingerprint = false;
            continue;
        }
        if (fields.size() <= 10 || fields[0] != "sig" || fields[1] != "!"
            || currentSubkey != requestedSubkeyFingerprint) {
            continue;
        }
        const QByteArray signer = fields[4].toUpper();
        const QByteArray signatureClass = fields[10].toLower();
        if (signer.endsWith(primaryKeyId)
            && signatureClass.startsWith("18")) {
            return true;
        }
    }
    return false;
}

bool hasValidPrimarySelfSignature(const QByteArray &checkedListing,
                                  const QString &primaryFingerprint)
{
    const QByteArray primaryKeyId = primaryFingerprint.right(16).toLatin1();
    for (const QByteArray &line : checkedListing.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.size() <= 10 || fields[0] != "sig" || fields[1] != "!") {
            continue;
        }
        const QByteArray signer = fields[4].toUpper();
        const QByteArray signatureClass = fields[10].toLower();
        if (signer.endsWith(primaryKeyId)
            && signatureClass.startsWith("13")) {
            return true;
        }
    }
    return false;
}
}

SigningKeyRecovery::SigningKeyRecovery(SigningKeyRecoveryConfig config,
                                       Fetcher fingerprintFetcher,
                                       Fetcher certificateFetcher,
                                       Runner runner)
    : m_config(std::move(config))
    , m_fingerprintFetcher(std::move(fingerprintFetcher))
    , m_certificateFetcher(std::move(certificateFetcher))
    , m_runner(std::move(runner))
{
}

QString SigningKeyRecovery::normalizeFingerprint(const QByteArray &value)
{
    QByteArray candidate = value;
    if (candidate.endsWith('\n')) {
        candidate.chop(1);
    }
    if (candidate.size() != 40) {
        return {};
    }
    static const QRegularExpression fingerprint(
        QStringLiteral("^[0-9A-F]{40}$"));
    const QString text = QString::fromLatin1(candidate);
    return fingerprint.match(text).hasMatch() ? text : QString{};
}

QString SigningKeyRecovery::requestedFingerprint(const QString &pacmanOutput)
{
    QString plain = pacmanOutput;
    plain.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;]*[A-Za-z]")));
    static const QRegularExpression request(
        QStringLiteral("(?:unknown[ -]key|signing[ -]key|key)[^\\n\\r]*?"
                       "((?:[0-9A-Fa-f][[:space:]:-]*){40})"
                       "(?![0-9A-Fa-f])"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = request.match(plain);
    if (!match.hasMatch()) {
        return {};
    }
    QString candidate = match.captured(1);
    candidate.remove(QRegularExpression(QStringLiteral("[:\\-\\s]")));
    return normalizedRecordFingerprint(candidate.toLatin1());
}

QString SigningKeyRecovery::repositoryName(const QString &pacmanOutput)
{
    QString plain = pacmanOutput;
    plain.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]")));
    static const QRegularExpression repository(
        QStringLiteral("(?:^|[\\r\\n])\\s*error:\\s*([A-Za-z0-9@._+:-]+):"
                       "\\s*key\\s*[\\\"']?[0-9A-Fa-f]{40}[\\\"']?"
                       "\\s+is unknown(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = repository.match(plain);
    return match.hasMatch() ? match.captured(1).toLower() : QString{};
}

bool SigningKeyRecovery::containsUnknownKeyReport(const QString &pacmanOutput)
{
    QString plain = pacmanOutput;
    plain.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;]*[A-Za-z]")));
    const QString lower = plain.toLower();
    return lower.contains(QStringLiteral("unknown signing key"))
        || lower.contains(QStringLiteral("unknown key"))
        || (lower.contains(QStringLiteral("is unknown"))
            && !requestedFingerprint(plain).isEmpty())
        || lower.contains(QStringLiteral("required key missing from keyring"))
        || lower.contains(QStringLiteral("key could not be looked up remotely"));
}

SigningKeyCommandResult SigningKeyRecovery::run(
    const QString &program, const QStringList &arguments) const
{
    return m_runner ? m_runner(program, arguments) : SigningKeyCommandResult{};
}

SigningKeyRecoveryResult SigningKeyRecovery::recover(
    const QString &pacmanOutput) const
{
    SigningKeyRecoveryResult result;
    result.repository = repositoryName(pacmanOutput);
    if (result.repository.isEmpty()) {
        result.category = QStringLiteral("repository-unidentified");
        return result;
    }
    if (result.repository != QStringLiteral("fluffnet")) {
        result.category = QStringLiteral("repository-not-fluffnet");
        return result;
    }
    result.requestedFingerprint = requestedFingerprint(pacmanOutput);
    if (result.requestedFingerprint.isEmpty()) {
        result.category = QStringLiteral("requested-fingerprint-invalid");
        return result;
    }

    QString fetchFailure;
    const QByteArray response = m_fingerprintFetcher
        ? m_fingerprintFetcher(&fetchFailure) : QByteArray{};
    if (!fetchFailure.isEmpty()) {
        result.category = fetchFailure;
        return result;
    }
    if (response.size() > m_config.maximumFingerprintResponseBytes) {
        result.category = QStringLiteral("fingerprint-response-oversized");
        return result;
    }
    result.expectedFingerprint = normalizeFingerprint(response);
    if (result.expectedFingerprint.isEmpty()) {
        result.category = QStringLiteral("fingerprint-response-malformed");
        return result;
    }

    QString certificateFetchFailure;
    const QByteArray certificateResponse = m_certificateFetcher
        ? m_certificateFetcher(&certificateFetchFailure) : QByteArray{};
    if (!certificateFetchFailure.isEmpty()) {
        result.category = certificateFetchFailure;
        return result;
    }
    if (certificateResponse.isEmpty()) {
        result.category = QStringLiteral("certificate-response-empty");
        return result;
    }
    if (certificateResponse.size() > m_config.maximumCertificateResponseBytes) {
        result.category = QStringLiteral("certificate-response-oversized");
        return result;
    }

    QTemporaryDir temporaryDirectory(
        QDir::tempPath() + QStringLiteral("/flufflinux-update-key-XXXXXX"));
    temporaryDirectory.setAutoRemove(true);
    if (!temporaryDirectory.isValid()
        || !QFile::setPermissions(temporaryDirectory.path(),
             QFileDevice::ReadOwner | QFileDevice::WriteOwner
                 | QFileDevice::ExeOwner)) {
        result.category = QStringLiteral("temporary-keyring-failed");
        return result;
    }

    const QStringList common{
        QStringLiteral("--batch"), QStringLiteral("--no-tty"),
        QStringLiteral("--homedir"), temporaryDirectory.path(),
    };
    const QString downloadedCertificatePath =
        temporaryDirectory.path() + QStringLiteral("/downloaded-certificate.asc");
    QFile downloadedCertificate(downloadedCertificatePath);
    if (!downloadedCertificate.open(QIODevice::WriteOnly)
        || downloadedCertificate.write(certificateResponse)
            != certificateResponse.size()
        || !downloadedCertificate.flush()) {
        result.category = QStringLiteral("certificate-write-failed");
        return result;
    }
    downloadedCertificate.close();

    QStringList packets = common;
    packets << QStringLiteral("--list-packets") << downloadedCertificatePath;
    const SigningKeyCommandResult packetListing = run(m_config.gpgPath, packets);
    const QByteArray lowerPacketListing = packetListing.output.toLower();
    if (!commandSucceeded(packetListing)) {
        result.category = QStringLiteral("certificate-parse-failed");
        return result;
    }
    if (lowerPacketListing.contains("secret key packet")
        || lowerPacketListing.contains("secret sub key packet")) {
        result.category = QStringLiteral("certificate-contains-private-key");
        return result;
    }

    QStringList import = common;
    import << QStringLiteral("--import") << downloadedCertificatePath;
    if (!commandSucceeded(run(m_config.gpgPath, import))) {
        result.category = QStringLiteral("certificate-import-failed");
        return result;
    }

    QStringList list = common;
    list << QStringLiteral("--with-colons") << QStringLiteral("--fixed-list-mode")
         << QStringLiteral("--fingerprint") << QStringLiteral("--fingerprint")
         << QStringLiteral("--list-keys") << result.requestedFingerprint;
    const SigningKeyCommandResult listing = run(m_config.gpgPath, list);
    if (!commandSucceeded(listing)) {
        result.category = QStringLiteral("certificate-inspection-failed");
        return result;
    }
    const CertificateDescription certificate = describeCertificate(listing.output);
    result.receivedFingerprint = certificate.primary;
    if (certificate.primary.isEmpty()) {
        result.category = QStringLiteral("certificate-fingerprint-invalid");
        return result;
    }
    if (certificate.primary != result.expectedFingerprint) {
        result.category = QStringLiteral("primary-fingerprint-mismatch");
        return result;
    }
    if (!certificate.primaryUsable) {
        result.category = QStringLiteral("primary-key-unusable");
        return result;
    }

    const bool requestedPrimary =
        result.requestedFingerprint == certificate.primary;
    const bool requestedSubkey =
        certificate.allSubkeys.contains(result.requestedFingerprint);
    if (!requestedPrimary && !requestedSubkey) {
        result.category = QStringLiteral("requested-key-not-in-certificate");
        return result;
    }
    if (requestedSubkey
        && !certificate.usableSigningSubkeys.contains(
            result.requestedFingerprint)) {
        result.category = QStringLiteral("signing-key-unusable");
        return result;
    }
    QStringList check = common;
    check << QStringLiteral("--with-colons")
          << QStringLiteral("--fingerprint")
          << QStringLiteral("--fingerprint")
          << QStringLiteral("--check-sigs") << certificate.primary;
    const SigningKeyCommandResult checked = run(m_config.gpgPath, check);
    if (!commandSucceeded(checked)
        || !hasValidPrimarySelfSignature(checked.output,
                                         certificate.primary)) {
        result.category = QStringLiteral("certificate-signature-invalid");
        return result;
    }
    if (requestedSubkey
        && !hasValidSubkeyBinding(checked.output, certificate.primary,
                                  result.requestedFingerprint)) {
        result.category = QStringLiteral("subkey-binding-invalid");
        return result;
    }

    const QString certificatePath =
        temporaryDirectory.path() + QStringLiteral("/certificate.gpg");
    QStringList exportArguments = common;
    exportArguments << QStringLiteral("--output") << certificatePath
                    << QStringLiteral("--export") << certificate.primary;
    if (!commandSucceeded(run(m_config.gpgPath, exportArguments))
        || !QFileInfo(certificatePath).isFile()) {
        result.category = QStringLiteral("certificate-export-failed");
        return result;
    }

    const SigningKeyCommandResult existing = run(
        m_config.pacmanKeyPath,
        {QStringLiteral("--finger"), certificate.primary});
    const bool wasAlreadyPresent = commandSucceeded(existing);
    if (!wasAlreadyPresent
        && !commandSucceeded(run(m_config.pacmanKeyPath,
             {QStringLiteral("--add"), certificatePath}))) {
        result.category = QStringLiteral("key-import-failed");
        return result;
    }
    if (!commandSucceeded(run(m_config.pacmanKeyPath,
            {QStringLiteral("--lsign-key"), certificate.primary}))) {
        if (!wasAlreadyPresent) {
            run(m_config.pacmanKeyPath,
                {QStringLiteral("--delete"), certificate.primary});
        }
        result.category = QStringLiteral("key-trust-failed");
        return result;
    }

    result.recovered = true;
    return result;
}
