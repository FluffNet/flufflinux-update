#include "signingkeyrecovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <utility>

namespace
{
bool commandSucceeded(const SigningKeyCommandResult &result)
{
    return result.started && result.finished && result.exitCode == 0;
}

bool keyDefinitelyMissing(const SigningKeyCommandResult &result)
{
    if (!result.started || !result.finished || result.exitCode == 0) {
        return false;
    }
    const QByteArray lower = result.output.toLower();
    return lower.contains("no public key")
        || lower.contains("key not found")
        || (lower.contains("the key identified by")
            && lower.contains("could not be found locally"));
}

QString pacmanGpgDirectory(const SigningKeyCommandResult &result,
                           int maximumResponseBytes)
{
    if (!commandSucceeded(result) || result.output.isEmpty()
        || result.output.size() > maximumResponseBytes
        || result.output.contains('\0')) {
        return {};
    }
    QByteArray path = result.output;
    if (path.endsWith('\n')) {
        path.chop(1);
        if (path.endsWith('\r')) {
            path.chop(1);
        }
    }
    if (path.isEmpty() || path.contains('\n') || path.contains('\r')) {
        return {};
    }
    const QString decoded = QString::fromLocal8Bit(path);
    if (!QDir::isAbsolutePath(decoded)) {
        return {};
    }
    const QString cleaned = QDir::cleanPath(decoded);
    if (cleaned == QStringLiteral("/") || !QFileInfo(cleaned).isDir()) {
        return {};
    }
    return cleaned;
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
    bool primarySigningCapable = false;
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
            const char validityCode = validity.isEmpty() ? '\0' : validity[0];
            const QByteArray rawCapabilities =
                fields.size() > 11 ? fields[11] : QByteArray{};
            pendingUsable = validityCode != 'r' && validityCode != 'e'
                && validityCode != 'd' && validityCode != 'i'
                && !rawCapabilities.contains('D');
            pendingSigningCapable = rawCapabilities.contains('s');
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
            description.primarySigningCapable = pendingSigningCapable;
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
        if (fields.size() <= 12 || fields[0] != "sig" || fields[1] != "!"
            || currentSubkey != requestedSubkeyFingerprint) {
            continue;
        }
        const QString signerFingerprint =
            normalizedRecordFingerprint(fields[12]);
        const QByteArray signatureClass = fields[10].toLower();
        if (signerFingerprint == primaryFingerprint
            && signatureClass.startsWith("18")) {
            return true;
        }
    }
    return false;
}

bool hasValidPrimarySelfSignature(const QByteArray &checkedListing,
                                  const QString &primaryFingerprint)
{
    for (const QByteArray &line : checkedListing.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.size() <= 12 || fields[0] != "sig" || fields[1] != "!") {
            continue;
        }
        const QString signerFingerprint =
            normalizedRecordFingerprint(fields[12]);
        const QByteArray signatureClass = fields[10].left(2).toLower();
        const bool acceptedClass = signatureClass == "10"
            || signatureClass == "11" || signatureClass == "12"
            || signatureClass == "13" || signatureClass == "1f";
        if (signerFingerprint == primaryFingerprint && acceptedClass) {
            return true;
        }
    }
    return false;
}

bool certificateSupportsRequest(const QByteArray &listing,
                                const QByteArray &checkedListing,
                                const QString &primaryFingerprint,
                                const QString &requestedFingerprint)
{
    const CertificateDescription certificate = describeCertificate(listing);
    if (certificate.primary != primaryFingerprint
        || !certificate.primaryUsable
        || !hasValidPrimarySelfSignature(checkedListing,
                                         primaryFingerprint)) {
        return false;
    }
    if (requestedFingerprint == primaryFingerprint) {
        return certificate.primarySigningCapable;
    }
    return certificate.allSubkeys.contains(requestedFingerprint)
        && certificate.usableSigningSubkeys.contains(requestedFingerprint)
        && hasValidSubkeyBinding(checkedListing, primaryFingerprint,
                                 requestedFingerprint);
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
                       "(?<![0-9A-Fa-f])([0-9A-Fa-f]{40})"
                       "(?![0-9A-Fa-f])"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> candidates;
    auto matches = request.globalMatch(plain);
    while (matches.hasNext()) {
        const QString candidate = normalizedRecordFingerprint(
            matches.next().captured(1).toLatin1());
        if (!candidate.isEmpty()) {
            candidates.insert(candidate);
        }
    }
    return candidates.size() == 1 ? *candidates.constBegin() : QString{};
}

QString SigningKeyRecovery::repositoryName(const QString &pacmanOutput)
{
    QString plain = pacmanOutput;
    plain.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]")));
    static const QRegularExpression repository(
        QStringLiteral("(?:^|[\\r\\n])\\s*error:\\s*([A-Za-z0-9@._+:-]+):"
                       "\\s*key\\s*[\\\"']?([0-9A-Fa-f]{40})[\\\"']?"
                       "\\s+is unknown(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> reports;
    auto matches = repository.globalMatch(plain);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString fingerprint = normalizedRecordFingerprint(
            match.captured(2).toLatin1());
        if (!fingerprint.isEmpty()) {
            reports.insert(match.captured(1).toLower()
                           + QChar(0x001f) + fingerprint);
        }
    }
    if (reports.size() != 1) {
        return {};
    }
    return reports.constBegin()->section(QChar(0x001f), 0, 0);
}

bool SigningKeyRecovery::containsUnknownKeyReport(const QString &pacmanOutput)
{
    QString plain = pacmanOutput;
    plain.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;]*[A-Za-z]")));
    const QString lower = plain.toLower();
    static const QRegularExpression repositoryUnknownKey(
        QStringLiteral("(?:^|[\\r\\n])\\s*error:\\s*"
                       "[A-Za-z0-9@._+:-]+:\\s*key\\s*"
                       "[\\\"']?[0-9A-Fa-f]{40}[\\\"']?"
                       "\\s+is unknown(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression remoteLookupFailure(
        QStringLiteral("(?:^|[\\r\\n])\\s*error:\\s*key\\s*"
                       "[\\\"']?[0-9A-Fa-f]{40}[\\\"']?"
                       "\\s+could not be looked up remotely(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return lower.contains(QStringLiteral("unknown signing key"))
        || lower.contains(QStringLiteral("unknown key"))
        || repositoryUnknownKey.match(plain).hasMatch()
        || lower.contains(QStringLiteral("required key missing from keyring"))
        || remoteLookupFailure.match(plain).hasMatch()
        || signingKeyImportPromptEnd(plain) >= 0;
}

qsizetype SigningKeyRecovery::signingKeyImportPromptEnd(
    const QString &pacmanOutput)
{
    static const QRegularExpression prompt(
        QStringLiteral("Import\\s+PGP\\s+key\\s+"
                       "([\\\"']?)(?:[0-9A-Fa-f]{40}|[0-9A-Fa-f]{16})\\1"
                       "(?:,[^\\r\\n]{0,512})?"
                       "\\?\\s*\\[Y/n\\]"));
    const auto match = prompt.match(pacmanOutput);
    return match.hasMatch() ? match.capturedEnd() : -1;
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
        QStringLiteral("--no-auto-key-retrieve"),
        QStringLiteral("--no-auto-key-import"),
        QStringLiteral("--require-cross-certification"),
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
    if (requestedPrimary && !certificate.primarySigningCapable) {
        result.category = QStringLiteral("signing-key-unusable");
        return result;
    }
    QStringList check = common;
    check << QStringLiteral("--with-colons")
          << QStringLiteral("--no-sig-cache")
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

    const SigningKeyCommandResult existingPrimary = run(
        m_config.pacmanKeyPath,
        {QStringLiteral("--finger"), certificate.primary});
    const bool primaryWasDefinitelyMissing =
        keyDefinitelyMissing(existingPrimary);
    if (!commandSucceeded(existingPrimary)
        && !primaryWasDefinitelyMissing) {
        result.category = QStringLiteral("keyring-inspection-failed");
        return result;
    }
    const auto rollBackNewPrimary = [&] {
        if (primaryWasDefinitelyMissing) {
            run(m_config.pacmanKeyPath,
                {QStringLiteral("--delete"), certificate.primary});
        }
    };

    // pacman-key --add updates an existing primary key as well as importing a
    // new one. Always merge the verified official certificate so rotated
    // signing subkeys and refreshed self-signatures reach Pacman's keyring.
    const SigningKeyCommandResult imported = run(
        m_config.pacmanKeyPath,
        {QStringLiteral("--add"), certificatePath});
    if (!commandSucceeded(imported)) {
        rollBackNewPrimary();
        result.category = QStringLiteral("key-import-failed");
        return result;
    }

    // Inspect Pacman's live keyring after the merge. Re-importing an exported
    // certificate into another keyring would discard local disabled-state
    // metadata, so resolve Pacman's configured GnuPG directory and validate
    // the exact live records in place.
    const SigningKeyCommandResult configuredGpgDirectory = run(
        m_config.pacmanConfPath, {QStringLiteral("gpgdir")});
    const QString liveGpgDirectory = pacmanGpgDirectory(
        configuredGpgDirectory, m_config.maximumPacmanConfResponseBytes);
    if (liveGpgDirectory.isEmpty()) {
        rollBackNewPrimary();
        result.category = QStringLiteral("keyring-inspection-failed");
        return result;
    }

    const QStringList liveCommon{
        QStringLiteral("--batch"), QStringLiteral("--no-tty"),
        QStringLiteral("--no-auto-check-trustdb"),
        QStringLiteral("--no-auto-key-retrieve"),
        QStringLiteral("--no-auto-key-import"),
        QStringLiteral("--require-cross-certification"),
        QStringLiteral("--homedir"), liveGpgDirectory,
    };
    QStringList liveList = liveCommon;
    liveList << QStringLiteral("--with-colons")
             << QStringLiteral("--fixed-list-mode")
             << QStringLiteral("--list-options")
             << QStringLiteral("show-unusable-subkeys")
             << QStringLiteral("--fingerprint")
             << QStringLiteral("--fingerprint")
             << QStringLiteral("--list-keys") << certificate.primary;
    const SigningKeyCommandResult liveListing = run(m_config.gpgPath, liveList);
    QStringList liveCheck = liveCommon;
    liveCheck << QStringLiteral("--with-colons")
              << QStringLiteral("--no-sig-cache")
              << QStringLiteral("--fingerprint")
              << QStringLiteral("--fingerprint")
              << QStringLiteral("--check-sigs") << certificate.primary;
    const SigningKeyCommandResult liveChecked = run(m_config.gpgPath, liveCheck);
    if (!commandSucceeded(liveListing)
        || !commandSucceeded(liveChecked)
        || !certificateSupportsRequest(
            liveListing.output, liveChecked.output,
            certificate.primary, result.requestedFingerprint)) {
        rollBackNewPrimary();
        result.category = QStringLiteral("requested-key-import-failed");
        return result;
    }

    if (!commandSucceeded(run(m_config.pacmanKeyPath,
            {QStringLiteral("--lsign-key"), certificate.primary}))) {
        rollBackNewPrimary();
        result.category = QStringLiteral("key-trust-failed");
        return result;
    }

    result.recovered = true;
    return result;
}
