#include "signingkeyrecovery.h"
#include "securitydiagnostics.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace
{
SigningKeyCommandResult execute(const QString &program,
                                const QStringList &arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    SigningKeyCommandResult result;
    result.started = process.waitForStarted(5000);
    result.finished = result.started && process.waitForFinished(30000);
    result.output = process.readAll();
    result.exitCode = result.finished ? process.exitCode() : -1;
    return result;
}

QString firstFingerprint(const QString &home)
{
    const auto result = execute(
        QStringLiteral("/usr/bin/gpg"),
        {QStringLiteral("--batch"), QStringLiteral("--homedir"), home,
         QStringLiteral("--with-colons"), QStringLiteral("--fingerprint"),
         QStringLiteral("--list-keys")});
    for (const QByteArray &line : result.output.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.size() > 9 && fields[0] == "fpr") {
            return QString::fromLatin1(fields[9]);
        }
    }
    return {};
}

QStringList primaryFingerprints(const QString &home)
{
    const auto result = execute(
        QStringLiteral("/usr/bin/gpg"),
        {QStringLiteral("--batch"), QStringLiteral("--homedir"), home,
         QStringLiteral("--with-colons"), QStringLiteral("--fingerprint"),
         QStringLiteral("--list-keys")});
    QStringList fingerprints;
    bool awaitingPrimary = false;
    for (const QByteArray &line : result.output.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (!fields.isEmpty() && fields[0] == "pub") {
            awaitingPrimary = true;
        } else if (awaitingPrimary && fields.size() > 9
                   && fields[0] == "fpr") {
            fingerprints << QString::fromLatin1(fields[9]);
            awaitingPrimary = false;
        }
    }
    return fingerprints;
}

QStringList subkeyFingerprints(const QString &home, const QString &primary)
{
    const auto result = execute(
        QStringLiteral("/usr/bin/gpg"),
        {QStringLiteral("--batch"), QStringLiteral("--homedir"), home,
         QStringLiteral("--with-colons"), QStringLiteral("--fingerprint"),
         QStringLiteral("--fingerprint"), QStringLiteral("--list-keys"),
         primary});
    QStringList fingerprints;
    bool awaitingSubkey = false;
    for (const QByteArray &line : result.output.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (!fields.isEmpty() && fields[0] == "sub") {
            awaitingSubkey = true;
        } else if (awaitingSubkey && fields.size() > 9
                   && fields[0] == "fpr") {
            fingerprints << QString::fromLatin1(fields[9]);
            awaitingSubkey = false;
        }
    }
    return fingerprints;
}

QByteArray exportedCertificate(const QString &home, const QString &primary,
                               bool secret = false)
{
    return execute(
        QStringLiteral("/usr/bin/gpg"),
        {QStringLiteral("--batch"), QStringLiteral("--homedir"), home,
         QStringLiteral("--armor"),
         secret ? QStringLiteral("--export-secret-keys")
                : QStringLiteral("--export"),
         primary}).output;
}

QString unknownKey(const QString &repository, const QString &fingerprint)
{
    return QStringLiteral("error: %1: key \"%2\" is unknown\n"
                          "error: keyring is not writable")
        .arg(repository, fingerprint);
}

QString pacmanSyncKeyserverFailure(const QString &repository,
                                   const QString &fingerprint)
{
    return QStringLiteral(
               ":: Synchronizing package databases...\n"
               " %1 downloading...\n"
               "error: %1: key \"%2\" is unknown\n"
               ":: Import PGP key \"%2\"? [Y/n]\n"
               "error: key \"%2\" could not be looked up remotely\n"
               "error: %1: key \"%2\" is unknown\n"
               ":: Import PGP key \"%2\"? [Y/n]\n"
               "error: key \"%2\" could not be looked up remotely\n"
               "error: failed to synchronize all databases (unexpected error)\n")
        .arg(repository, fingerprint);
}

SigningKeyCommandResult completed(int exitCode = 0)
{
    SigningKeyCommandResult result;
    result.started = true;
    result.finished = true;
    result.exitCode = exitCode;
    return result;
}

SigningKeyCommandResult completedWithOutput(const QByteArray &output)
{
    SigningKeyCommandResult result = completed();
    result.output = output;
    return result;
}

SigningKeyCommandResult missingPublicKey()
{
    SigningKeyCommandResult result = completed(2);
    result.output = QByteArrayLiteral(
        "The key identified by 0123456789ABCDEF could not be found locally.\n");
    return result;
}

QByteArray mutateFirstSubkey(const QByteArray &listing,
                             const QByteArray &validity,
                             bool removeSigningCapability)
{
    QList<QByteArray> lines = listing.split('\n');
    for (QByteArray &line : lines) {
        QList<QByteArray> fields = line.split(':');
        if (fields.isEmpty() || fields[0] != "sub") {
            continue;
        }
        if (fields.size() > 1 && !validity.isEmpty()) {
            fields[1] = validity;
        }
        if (fields.size() > 11 && removeSigningCapability) {
            fields[11].replace("s", "");
            fields[11].replace("S", "");
        }
        line = fields.join(':');
        break;
    }
    return lines.join('\n');
}

QByteArray disablePrimary(const QByteArray &listing)
{
    QList<QByteArray> lines = listing.split('\n');
    for (QByteArray &line : lines) {
        QList<QByteArray> fields = line.split(':');
        if (fields.isEmpty() || fields[0] != "pub") {
            continue;
        }
        if (fields.size() > 11 && !fields[11].contains('D')) {
            fields[11].append('D');
        }
        line = fields.join(':');
        break;
    }
    return lines.join('\n');
}

QByteArray removeSignatureClass(const QByteArray &listing,
                                const QByteArray &signatureClassPrefix)
{
    QList<QByteArray> retained;
    for (const QByteArray &line : listing.split('\n')) {
        const QList<QByteArray> fields = line.split(':');
        if (fields.size() > 10 && fields[0] == "sig"
            && fields[10].toLower().startsWith(signatureClassPrefix)) {
            continue;
        }
        retained.append(line);
    }
    return retained.join('\n');
}

QByteArray rewriteSignatureClass(const QByteArray &listing,
                                 const QByteArray &fromPrefix,
                                 const QByteArray &toPrefix)
{
    QList<QByteArray> lines = listing.split('\n');
    for (QByteArray &line : lines) {
        QList<QByteArray> fields = line.split(':');
        if (fields.size() <= 10 || fields[0] != "sig"
            || !fields[10].toLower().startsWith(fromPrefix)) {
            continue;
        }
        fields[10].replace(0, 2, toPrefix);
        line = fields.join(':');
    }
    return lines.join('\n');
}

QByteArray rewriteSignatureIssuer(const QByteArray &listing,
                                  const QByteArray &signatureClassPrefix,
                                  const QString &issuerFingerprint)
{
    QList<QByteArray> lines = listing.split('\n');
    for (QByteArray &line : lines) {
        QList<QByteArray> fields = line.split(':');
        if (fields.size() <= 12 || fields[0] != "sig"
            || !fields[10].toLower().startsWith(signatureClassPrefix)) {
            continue;
        }
        fields[12] = issuerFingerprint.toLatin1();
        line = fields.join(':');
    }
    return lines.join('\n');
}
}

class SigningKeyRecoveryTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(QStringLiteral("/usr/bin/gpg"))) {
            QSKIP("GnuPG is required for the disposable certificate tests");
        }
        QVERIFY(m_keys.isValid());
        QVERIFY(QFile::setPermissions(
            m_keys.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner));

        const QStringList common{
            QStringLiteral("--batch"), QStringLiteral("--homedir"),
            m_keys.path(), QStringLiteral("--pinentry-mode"),
            QStringLiteral("loopback"), QStringLiteral("--passphrase"),
            QString(),
        };
        QStringList generate = common;
        generate << QStringLiteral("--quick-generate-key")
                 << QStringLiteral("Expected Test Key <expected.invalid>")
                 << QStringLiteral("ed25519") << QStringLiteral("cert")
                 << QStringLiteral("1d");
        QCOMPARE(execute(QStringLiteral("/usr/bin/gpg"), generate).exitCode, 0);
        m_expectedPrimary = firstFingerprint(m_keys.path());
        QCOMPARE(m_expectedPrimary.size(), 40);

        for (int index = 0; index < 2; ++index) {
            QStringList add = common;
            add << QStringLiteral("--quick-add-key") << m_expectedPrimary
                << QStringLiteral("ed25519") << QStringLiteral("sign")
                << QStringLiteral("1d");
            QCOMPARE(execute(QStringLiteral("/usr/bin/gpg"), add).exitCode, 0);
        }
        m_expectedSubkeys = subkeyFingerprints(m_keys.path(), m_expectedPrimary);
        QCOMPARE(m_expectedSubkeys.size(), 2);

        QStringList unrelated = common;
        unrelated << QStringLiteral("--quick-generate-key")
                  << QStringLiteral("Unrelated Test Key <unrelated.invalid>")
                  << QStringLiteral("ed25519") << QStringLiteral("cert")
                  << QStringLiteral("1d");
        QCOMPARE(execute(QStringLiteral("/usr/bin/gpg"), unrelated).exitCode, 0);
        const QStringList primaries = primaryFingerprints(m_keys.path());
        QCOMPARE(primaries.size(), 2);
        m_unrelatedPrimary = primaries[1];

        QStringList unrelatedSigning = common;
        unrelatedSigning << QStringLiteral("--quick-add-key")
                         << m_unrelatedPrimary << QStringLiteral("ed25519")
                         << QStringLiteral("sign") << QStringLiteral("1d");
        QCOMPARE(execute(QStringLiteral("/usr/bin/gpg"), unrelatedSigning).exitCode,
                 0);
        m_unrelatedSubkey =
            subkeyFingerprints(m_keys.path(), m_unrelatedPrimary).constFirst();
    }

    void normalizeFingerprint_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<bool>("valid");
        const QByteArray fingerprint = m_expectedPrimary.toLatin1();
        QTest::newRow("exact") << fingerprint << true;
        QTest::newRow("one-trailing-newline") << fingerprint + '\n' << true;
        QTest::newRow("lowercase") << fingerprint.toLower() << false;
        QTest::newRow("leading-space") << ' ' + fingerprint << false;
        QTest::newRow("embedded-whitespace")
            << fingerprint.left(20) + " \t" + fingerprint.mid(20) << false;
        QTest::newRow("crlf") << fingerprint + "\r\n" << false;
        QTest::newRow("empty") << QByteArray{} << false;
        QTest::newRow("malformed") << QByteArray("not-a-fingerprint") << false;
        QTest::newRow("multiple") << fingerprint + '\n' + fingerprint << false;
        QTest::newRow("oversized") << QByteArray(5000, 'A') << false;
    }

    void normalizeFingerprint()
    {
        QFETCH(QByteArray, response);
        QFETCH(bool, valid);
        QCOMPARE(!SigningKeyRecovery::normalizeFingerprint(response).isEmpty(),
                 valid);
    }

    void acceptsCertifiedRotatedSubkeys_data()
    {
        QTest::addColumn<QString>("requested");
        QTest::newRow("current-subkey") << m_expectedSubkeys[0];
        QTest::newRow("rotated-subkey") << m_expectedSubkeys[1];
    }

    void acceptsCertifiedRotatedSubkeys()
    {
        QFETCH(QString, requested);
        QStringList operations;
        SigningKeyRecovery recovery = makeRecovery(
            m_expectedPrimary, m_expectedPrimary, &operations);
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QVERIFY2(result.recovered, qPrintable(result.category));
        QCOMPARE(result.repository, QStringLiteral("fluffnet"));
        QCOMPARE(result.receivedFingerprint, m_expectedPrimary);
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(operations.contains(QStringLiteral("--lsign-key")));
    }

    void rejectsCertOnlyPrimaryAsSigningKey()
    {
        QStringList operations;
        SigningKeyRecovery recovery = makeRecovery(
            m_expectedPrimary, m_expectedPrimary, &operations);
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedPrimary));
        QCOMPARE(result.category, QStringLiteral("signing-key-unusable"));
        QVERIFY(operations.isEmpty());
    }

    void refreshesMissingSubkeyWhenPrimaryAlreadyExists()
    {
        const QString requested = m_expectedSubkeys[1];
        QStringList operations;
        bool certificateAdded = false;
        const QString sourceHome = m_keys.path();
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [this, requested, sourceHome, &operations, &certificateAdded](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (!program.endsWith(QStringLiteral("pacman-key"))) {
                    return execute(program, arguments);
                }
                const QString operation = arguments.value(0);
                const QString target = arguments.value(1);
                operations << operation + QLatin1Char(':') + target;
                if (operation == QStringLiteral("--finger")) {
                    if (target == m_expectedPrimary) {
                        return completed();
                    }
                    return completed(certificateAdded && target == requested
                                         ? 0 : 2);
                }
                if (operation == QStringLiteral("--add")) {
                    certificateAdded = true;
                }
                return completed();
            });

        const auto result = recovery.recover(
            pacmanSyncKeyserverFailure(QStringLiteral("fluffnet"), requested));
        QVERIFY2(result.recovered, qPrintable(result.category));
        QVERIFY(certificateAdded);
        QVERIFY(operations.contains(
            QStringLiteral("--finger:") + m_expectedPrimary));
        QVERIFY(std::any_of(operations.cbegin(), operations.cend(),
                            [](const QString &operation) {
            return operation.startsWith(QStringLiteral("--add:"));
        }));
        QVERIFY(operations.contains(
            QStringLiteral("--lsign-key:") + m_expectedPrimary));
        QVERIFY(!operations.join(QLatin1Char('\n')).contains(
            QStringLiteral("--delete:")));
    }

    void primaryRolloverAfterLongOfflineState_data()
    {
        QTest::addColumn<bool>("trustSucceeds");
        QTest::newRow("new-primary-is-added-beside-obsolete-primary") << true;
        QTest::newRow("failed-trust-removes-only-new-primary") << false;
    }

    void primaryRolloverAfterLongOfflineState()
    {
        QFETCH(bool, trustSucceeds);
        QTemporaryDir liveKeyring;
        QVERIFY(liveKeyring.isValid());

        const QString oldCertificatePath =
            liveKeyring.path() + QStringLiteral("/obsolete-primary.asc");
        QFile oldCertificate(oldCertificatePath);
        const QByteArray oldCertificateData =
            exportedCertificate(m_keys.path(), m_unrelatedPrimary);
        QVERIFY(oldCertificate.open(QIODevice::WriteOnly));
        QCOMPARE(oldCertificate.write(oldCertificateData),
                 oldCertificateData.size());
        oldCertificate.close();
        QCOMPARE(execute(
                     QStringLiteral("/usr/bin/gpg"),
                     {QStringLiteral("--batch"), QStringLiteral("--homedir"),
                      liveKeyring.path(), QStringLiteral("--import"),
                      oldCertificatePath})
                     .exitCode,
                 0);
        QCOMPARE(primaryFingerprints(liveKeyring.path()),
                 QStringList{m_unrelatedPrimary});

        QStringList operations;
        const QString sourceHome = m_keys.path();
        const QString newPrimary = m_expectedPrimary;
        SigningKeyRecovery recovery(
            testConfig(),
            [newPrimary](QString *) { return newPrimary.toLatin1(); },
            [sourceHome, newPrimary](QString *) {
                return exportedCertificate(sourceHome, newPrimary);
            },
            [liveHome = liveKeyring.path(), trustSucceeds, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(liveHome.toLocal8Bit() + '\n');
                }
                if (!program.endsWith(QStringLiteral("pacman-key"))) {
                    return execute(program, arguments);
                }

                const QString operation = arguments.value(0);
                const QString target = arguments.value(1);
                operations << operation + QLatin1Char(':') + target;
                if (operation == QStringLiteral("--finger")) {
                    return execute(
                        QStringLiteral("/usr/bin/gpg"),
                        {QStringLiteral("--batch"),
                         QStringLiteral("--homedir"), liveHome,
                         QStringLiteral("--with-colons"),
                         QStringLiteral("--fingerprint"),
                         QStringLiteral("--list-keys"), target});
                }
                if (operation == QStringLiteral("--add")) {
                    return execute(
                        QStringLiteral("/usr/bin/gpg"),
                        {QStringLiteral("--batch"),
                         QStringLiteral("--homedir"), liveHome,
                         QStringLiteral("--import"), target});
                }
                if (operation == QStringLiteral("--lsign-key")) {
                    return completed(trustSucceeds ? 0 : 1);
                }
                if (operation == QStringLiteral("--delete")) {
                    return execute(
                        QStringLiteral("/usr/bin/gpg"),
                        {QStringLiteral("--batch"), QStringLiteral("--yes"),
                         QStringLiteral("--homedir"), liveHome,
                         QStringLiteral("--delete-key"), target});
                }
                return completed(1);
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[1]));
        QCOMPARE(result.recovered, trustSucceeds);
        QCOMPARE(result.category,
                 trustSucceeds ? QString{} : QStringLiteral("key-trust-failed"));

        const QStringList livePrimaries =
            primaryFingerprints(liveKeyring.path());
        QVERIFY(livePrimaries.contains(m_unrelatedPrimary));
        QCOMPARE(livePrimaries.contains(m_expectedPrimary), trustSucceeds);
        QVERIFY(std::any_of(operations.cbegin(), operations.cend(),
                            [](const QString &operation) {
            return operation.startsWith(QStringLiteral("--add:"));
        }));
        QVERIFY(operations.contains(
            QStringLiteral("--lsign-key:") + m_expectedPrimary));
        QCOMPARE(operations.contains(
                     QStringLiteral("--delete:") + m_expectedPrimary),
                 !trustSucceeds);
        QVERIFY(!operations.contains(
            QStringLiteral("--delete:") + m_unrelatedPrimary));
    }

    void failsIfRequestedSubkeyIsStillMissingAfterMerge()
    {
        const QString requested = m_expectedSubkeys[1];
        QStringList operations;
        bool certificateAdded = false;
        const QString sourceHome = m_keys.path();
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [this, sourceHome, &operations, &certificateAdded](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--finger")) {
                        return completed(arguments.value(1) == m_expectedPrimary
                                             ? 0 : 2);
                    }
                    if (operation == QStringLiteral("--add")) {
                        certificateAdded = true;
                    }
                    return completed();
                }
                if (certificateAdded
                    && arguments.contains(QStringLiteral("--list-keys"))) {
                    return completed(2);
                }
                return execute(program, arguments);
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QCOMPARE(result.category,
                 QStringLiteral("requested-key-import-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void doesNotDeleteExistingPrimaryWhenTrustRefreshFails()
    {
        const QString requested = m_expectedSubkeys[1];
        QStringList operations;
        const QString sourceHome = m_keys.path();
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [sourceHome, this, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (!program.endsWith(QStringLiteral("pacman-key"))) {
                    return execute(program, arguments);
                }
                const QString operation = arguments.value(0);
                operations << operation;
                return completed(operation == QStringLiteral("--lsign-key")
                                     ? 1 : 0);
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QCOMPARE(result.category, QStringLiteral("key-trust-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void rejectsUnusableSigningSubkeyAfterPacmanMerge_data()
    {
        QTest::addColumn<QByteArray>("validity");
        QTest::addColumn<bool>("removeSigningCapability");
        QTest::newRow("expired") << QByteArray("e") << false;
        QTest::newRow("expired-with-future-suffix")
            << QByteArray("eFuture") << false;
        QTest::newRow("revoked") << QByteArray("r") << false;
        QTest::newRow("revoked-with-future-suffix")
            << QByteArray("rFuture") << false;
        QTest::newRow("disabled") << QByteArray("d") << false;
        QTest::newRow("invalid-with-future-suffix")
            << QByteArray("iFuture") << false;
        QTest::newRow("non-signing") << QByteArray{} << true;
    }

    void rejectsUnusableSigningSubkeyAfterPacmanMerge()
    {
        QFETCH(QByteArray, validity);
        QFETCH(bool, removeSigningCapability);
        const QString requested = m_expectedSubkeys[0];
        const QString sourceHome = m_keys.path();
        bool certificateAdded = false;
        QStringList operations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [this, sourceHome, validity, removeSigningCapability,
             &certificateAdded, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--add")) {
                        certificateAdded = true;
                    }
                    return completed();
                }
                SigningKeyCommandResult command = execute(program, arguments);
                if (certificateAdded
                    && arguments.contains(QStringLiteral("--list-keys"))) {
                    command.output = mutateFirstSubkey(
                        command.output, validity, removeSigningCapability);
                }
                return command;
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QCOMPARE(result.category,
                 QStringLiteral("requested-key-import-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void rejectsDisabledPrimaryAfterPacmanMerge()
    {
        const QString requested = m_expectedSubkeys[0];
        const QString sourceHome = m_keys.path();
        bool certificateAdded = false;
        QStringList operations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [sourceHome, &certificateAdded, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--add")) {
                        certificateAdded = true;
                    }
                    return completed();
                }
                SigningKeyCommandResult command = execute(program, arguments);
                if (certificateAdded
                    && arguments.contains(QStringLiteral("--list-keys"))) {
                    command.output = disablePrimary(command.output);
                }
                return command;
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QCOMPARE(result.category,
                 QStringLiteral("requested-key-import-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void rejectsMissingBindingAfterPacmanMerge()
    {
        const QString requested = m_expectedSubkeys[0];
        const QString sourceHome = m_keys.path();
        bool certificateAdded = false;
        QStringList operations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [sourceHome, &certificateAdded, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--add")) {
                        certificateAdded = true;
                    }
                    return completed();
                }
                SigningKeyCommandResult command = execute(program, arguments);
                if (certificateAdded
                    && arguments.contains(QStringLiteral("--check-sigs"))) {
                    command.output = removeSignatureClass(
                        command.output, QByteArrayLiteral("18"));
                }
                return command;
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), requested));
        QCOMPARE(result.category,
                 QStringLiteral("requested-key-import-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void rejectsUnsafePacmanGpgDirectoryResponses_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("command-failure") << QByteArray{} << 1;
        QTest::newRow("empty") << QByteArray{} << 0;
        QTest::newRow("relative") << QByteArray("relative/keyring\n") << 0;
        QTest::newRow("multiple-lines")
            << QByteArray("/tmp/one\n/tmp/two\n") << 0;
        QTest::newRow("root") << QByteArray("/\n") << 0;
        QTest::newRow("missing-directory")
            << QByteArray("/definitely/missing/flu-keyring\n") << 0;
        QTest::newRow("embedded-nul")
            << QByteArray("/tmp/keyring\0suffix", 19) << 0;
        QTest::newRow("oversized") << QByteArray(4097, 'A') << 0;
    }

    void rejectsUnsafePacmanGpgDirectoryResponses()
    {
        QFETCH(QByteArray, response);
        QFETCH(int, exitCode);
        const QString sourceHome = m_keys.path();
        QStringList operations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [response, exitCode, &operations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    SigningKeyCommandResult result = completed(exitCode);
                    result.output = response;
                    return result;
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    operations << arguments.value(0);
                    return completed();
                }
                return execute(program, arguments);
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("keyring-inspection-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
        QVERIFY(!operations.contains(QStringLiteral("--delete")));
    }

    void inspectionFailureRollsBackNewPrimary()
    {
        const QString sourceHome = m_keys.path();
        QStringList operations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [this, &operations](const QString &program,
                                const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completed(1);
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--finger")
                        && arguments.value(1) == m_expectedPrimary) {
                        return missingPublicKey();
                    }
                    return completed();
                }
                return execute(program, arguments);
            });

        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("keyring-inspection-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(operations.contains(QStringLiteral("--delete")));
        QVERIFY(!operations.contains(QStringLiteral("--lsign-key")));
    }

    void rejectsUnrelatedPrimaryAndSubkey()
    {
        QStringList operations;
        SigningKeyRecovery recovery = makeRecovery(
            m_expectedPrimary, m_unrelatedPrimary, &operations);
        auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_unrelatedPrimary));
        QCOMPARE(result.category, QStringLiteral("primary-fingerprint-mismatch"));
        QVERIFY(operations.isEmpty());

        operations.clear();
        recovery = makeRecovery(m_expectedPrimary, m_unrelatedPrimary,
                                &operations);
        result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_unrelatedSubkey));
        QCOMPARE(result.category, QStringLiteral("primary-fingerprint-mismatch"));
        QVERIFY(operations.isEmpty());
    }

    void repositoryIsolation()
    {
        const QStringList repositories{
            QStringLiteral("core"), QStringLiteral("extra"),
            QStringLiteral("thirdparty"), QStringLiteral("local"),
            QStringLiteral("closed-network"),
        };
        for (const QString &repository : repositories) {
            int fingerprintFetches = 0;
            int certificateFetches = 0;
            int pacmanOperations = 0;
            SigningKeyRecovery recovery(
                testConfig(),
                [this, &fingerprintFetches](QString *) {
                    ++fingerprintFetches;
                    return m_expectedPrimary.toLatin1();
                },
                [this, &certificateFetches](QString *) {
                    ++certificateFetches;
                    return exportedCertificate(m_keys.path(), m_expectedPrimary);
                },
                [&pacmanOperations](const QString &program,
                                    const QStringList &arguments) {
                    if (program.endsWith(QStringLiteral("pacman-key"))) {
                        ++pacmanOperations;
                    }
                    return execute(program, arguments);
                });
            const auto result = recovery.recover(
                unknownKey(repository, m_expectedSubkeys[0]));
            QCOMPARE(result.category, QStringLiteral("repository-not-fluffnet"));
            QCOMPARE(result.repository, repository);
            QCOMPARE(fingerprintFetches, 0);
            QCOMPARE(certificateFetches, 0);
            QCOMPARE(pacmanOperations, 0);
        }

        int fetches = 0;
        SigningKeyRecovery unidentified(
            testConfig(),
            [&fetches](QString *) { ++fetches; return QByteArray{}; },
            [&fetches](QString *) { ++fetches; return QByteArray{}; },
            [](const QString &, const QStringList &) { return completed(); });
        const auto result = unidentified.recover(
            QStringLiteral("unknown signing key %1").arg(m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("repository-unidentified"));
        QCOMPARE(fetches, 0);
    }

    void endpointAndCertificateFailuresAreSafe()
    {
        SigningKeyRecovery fingerprintFailure(
            testConfig(),
            [](QString *category) {
                *category = QStringLiteral("fingerprint-endpoint-unavailable");
                return QByteArray{};
            },
            [](QString *) { return QByteArray{}; },
            [](const QString &, const QStringList &) { return completed(); });
        auto result = fingerprintFailure.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("fingerprint-endpoint-unavailable"));

        SigningKeyRecovery certificateFailure(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [](QString *category) {
                *category = QStringLiteral("certificate-endpoint-unavailable");
                return QByteArray{};
            },
            [](const QString &, const QStringList &) { return completed(); });
        result = certificateFailure.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("certificate-endpoint-unavailable"));

        SigningKeyRecovery emptyCertificate(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [](QString *) { return QByteArray{}; },
            [](const QString &, const QStringList &) { return completed(); });
        result = emptyCertificate.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("certificate-response-empty"));

        SigningKeyRecovery oversizedCertificate(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [](QString *) { return QByteArray(256 * 1024 + 1, 'A'); },
            [](const QString &, const QStringList &) { return completed(); });
        result = oversizedCertificate.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("certificate-response-oversized"));

        SigningKeyRecovery malformedCertificate(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [](QString *) { return QByteArray("not an OpenPGP certificate"); },
            [](const QString &program, const QStringList &arguments) {
                return execute(program, arguments);
            });
        result = malformedCertificate.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("certificate-parse-failed"));
    }

    void rejectsPrivateKeyPackets()
    {
        int pacmanOperations = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [this](QString *) {
                return exportedCertificate(m_keys.path(), m_expectedPrimary,
                                           true);
            },
            [&pacmanOperations](const QString &program,
                                const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    ++pacmanOperations;
                }
                return execute(program, arguments);
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("certificate-contains-private-key"));
        QCOMPARE(pacmanOperations, 0);
    }

    void fingerprintResponsesFailClosed_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<QString>("category");
        const QByteArray fingerprint = m_expectedPrimary.toLatin1();
        QTest::newRow("empty")
            << QByteArray{} << QStringLiteral("fingerprint-response-malformed");
        QTest::newRow("lowercase")
            << fingerprint.toLower()
            << QStringLiteral("fingerprint-response-malformed");
        QTest::newRow("multiple")
            << fingerprint + '\n' + fingerprint
            << QStringLiteral("fingerprint-response-malformed");
        QTest::newRow("whitespace")
            << fingerprint.left(20) + ' ' + fingerprint.mid(20)
            << QStringLiteral("fingerprint-response-malformed");
        QTest::newRow("oversized")
            << QByteArray(4097, 'A')
            << QStringLiteral("fingerprint-response-oversized");
    }

    void fingerprintResponsesFailClosed()
    {
        QFETCH(QByteArray, response);
        QFETCH(QString, category);
        int certificateFetches = 0;
        int commands = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [response](QString *) { return response; },
            [&certificateFetches](QString *) {
                ++certificateFetches;
                return QByteArray{};
            },
            [&commands](const QString &, const QStringList &) {
                ++commands;
                return completed();
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, category);
        QCOMPARE(certificateFetches, 0);
        QCOMPARE(commands, 0);
    }

    void certificateImportFailureDoesNotReachPacman()
    {
        int pacmanOperations = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [this](QString *) {
                return exportedCertificate(m_keys.path(), m_expectedPrimary);
            },
            [&pacmanOperations](const QString &program,
                                const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    ++pacmanOperations;
                }
                if (program.endsWith(QStringLiteral("gpg"))
                    && arguments.contains(QStringLiteral("--import"))) {
                    return completed(1);
                }
                return execute(program, arguments);
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("certificate-import-failed"));
        QCOMPARE(pacmanOperations, 0);
    }

    void rejectsUnusableSigningSubkey_data()
    {
        QTest::addColumn<QByteArray>("validity");
        QTest::addColumn<bool>("removeSigningCapability");
        QTest::newRow("expired") << QByteArray("e") << false;
        QTest::newRow("expired-with-future-suffix")
            << QByteArray("eFuture") << false;
        QTest::newRow("revoked") << QByteArray("r") << false;
        QTest::newRow("revoked-with-future-suffix")
            << QByteArray("rFuture") << false;
        QTest::newRow("disabled") << QByteArray("d") << false;
        QTest::newRow("invalid") << QByteArray("i") << false;
        QTest::newRow("invalid-with-future-suffix")
            << QByteArray("iFuture") << false;
        QTest::newRow("non-signing") << QByteArray{} << true;
    }

    void rejectsUnusableSigningSubkey()
    {
        QFETCH(QByteArray, validity);
        QFETCH(bool, removeSigningCapability);
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [this](QString *) {
                return exportedCertificate(m_keys.path(), m_expectedPrimary);
            },
            [validity, removeSigningCapability](
                const QString &program, const QStringList &arguments) {
                SigningKeyCommandResult result = execute(program, arguments);
                if (arguments.contains(QStringLiteral("--list-keys"))) {
                    result.output = mutateFirstSubkey(
                        result.output, validity, removeSigningCapability);
                }
                return result;
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("signing-key-unusable"));
    }

    void rejectsMissingBindingAndSelfSignatures_data()
    {
        QTest::addColumn<QByteArray>("signatureClass");
        QTest::addColumn<QString>("category");
        QTest::newRow("missing-binding")
            << QByteArray("18") << QStringLiteral("subkey-binding-invalid");
        QTest::newRow("missing-self-signature")
            << QByteArray("13")
            << QStringLiteral("certificate-signature-invalid");
    }

    void rejectsMissingBindingAndSelfSignatures()
    {
        QFETCH(QByteArray, signatureClass);
        QFETCH(QString, category);
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [this](QString *) {
                return exportedCertificate(m_keys.path(), m_expectedPrimary);
            },
            [signatureClass](const QString &program,
                             const QStringList &arguments) {
                SigningKeyCommandResult result = execute(program, arguments);
                if (arguments.contains(QStringLiteral("--check-sigs"))) {
                    result.output =
                        removeSignatureClass(result.output, signatureClass);
                }
                return result;
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, category);
    }

    void acceptsPrimarySelfSignatureClasses_data()
    {
        QTest::addColumn<QByteArray>("signatureClass");
        QTest::newRow("generic-certification") << QByteArray("10");
        QTest::newRow("persona-certification") << QByteArray("11");
        QTest::newRow("casual-certification") << QByteArray("12");
        QTest::newRow("positive-certification") << QByteArray("13");
        QTest::newRow("direct-key-signature") << QByteArray("1f");
    }

    void acceptsPrimarySelfSignatureClasses()
    {
        QFETCH(QByteArray, signatureClass);
        const QString sourceHome = m_keys.path();
        QStringList pacmanOperations;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [sourceHome, signatureClass, &pacmanOperations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    pacmanOperations << operation;
                    return operation == QStringLiteral("--finger")
                        ? missingPublicKey() : completed();
                }
                SigningKeyCommandResult result = execute(program, arguments);
                if (arguments.contains(QStringLiteral("--check-sigs"))) {
                    result.output = rewriteSignatureClass(
                        result.output, QByteArrayLiteral("13"),
                        signatureClass);
                }
                return result;
            });
        const auto result = recovery.recover(unknownKey(
            QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QVERIFY2(result.recovered, qPrintable(result.category));
        QVERIFY(pacmanOperations.contains(QStringLiteral("--add")));
        QVERIFY(pacmanOperations.contains(QStringLiteral("--lsign-key")));
    }

    void rejectsBindingWithMismatchedFullIssuerFingerprint()
    {
        const QString sourceHome = m_keys.path();
        QString mismatchedIssuer = m_expectedPrimary;
        mismatchedIssuer[0] = mismatchedIssuer[0] == QLatin1Char('A')
            ? QLatin1Char('B') : QLatin1Char('A');
        QVERIFY(mismatchedIssuer != m_expectedPrimary);
        QCOMPARE(mismatchedIssuer.right(16), m_expectedPrimary.right(16));
        int pacmanOperations = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [mismatchedIssuer, &pacmanOperations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    ++pacmanOperations;
                }
                SigningKeyCommandResult result = execute(program, arguments);
                if (arguments.contains(QStringLiteral("--check-sigs"))) {
                    result.output = rewriteSignatureIssuer(
                        result.output, QByteArrayLiteral("18"),
                        mismatchedIssuer);
                }
                return result;
            });
        const auto result = recovery.recover(unknownKey(
            QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("subkey-binding-invalid"));
        QCOMPARE(pacmanOperations, 0);
    }

    void rejectsSelfSignatureWithMismatchedFullIssuerFingerprint()
    {
        const QString sourceHome = m_keys.path();
        QString mismatchedIssuer = m_expectedPrimary;
        mismatchedIssuer[0] = mismatchedIssuer[0] == QLatin1Char('A')
            ? QLatin1Char('B') : QLatin1Char('A');
        QCOMPARE(mismatchedIssuer.right(16), m_expectedPrimary.right(16));
        int pacmanOperations = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [mismatchedIssuer, &pacmanOperations](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    ++pacmanOperations;
                }
                SigningKeyCommandResult result = execute(program, arguments);
                if (arguments.contains(QStringLiteral("--check-sigs"))) {
                    result.output = rewriteSignatureIssuer(
                        result.output, QByteArrayLiteral("13"),
                        mismatchedIssuer);
                }
                return result;
            });
        const auto result = recovery.recover(unknownKey(
            QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("certificate-signature-invalid"));
        QCOMPARE(pacmanOperations, 0);
    }

    void indeterminatePrimaryProbeDoesNotMutate_data()
    {
        QTest::addColumn<bool>("started");
        QTest::addColumn<bool>("finished");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QByteArray>("output");
        QTest::newRow("failed-start")
            << false << false << -1 << QByteArray{};
        QTest::newRow("timeout")
            << true << false << -1 << QByteArray{};
        QTest::newRow("unrecognized-failure")
            << true << true << 2
            << QByteArrayLiteral("unrecognized localized failure");
    }

    void indeterminatePrimaryProbeDoesNotMutate()
    {
        QFETCH(bool, started);
        QFETCH(bool, finished);
        QFETCH(int, exitCode);
        QFETCH(QByteArray, output);
        const QString sourceHome = m_keys.path();
        QStringList pacmanOperations;
        int pacmanConfCalls = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [started, finished, exitCode, output, &pacmanOperations,
             &pacmanConfCalls](const QString &program,
                              const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    ++pacmanConfCalls;
                    return completed();
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    pacmanOperations << arguments.value(0);
                    SigningKeyCommandResult result;
                    result.started = started;
                    result.finished = finished;
                    result.exitCode = exitCode;
                    result.output = output;
                    return result;
                }
                return execute(program, arguments);
            });
        const auto result = recovery.recover(unknownKey(
            QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category,
                 QStringLiteral("keyring-inspection-failed"));
        QCOMPARE(pacmanOperations,
                 QStringList{QStringLiteral("--finger")});
        QCOMPARE(pacmanConfCalls, 0);
    }

    void rejectsShortKeyIdsAndLeavesNormalFailuresAlone()
    {
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(
                     QStringLiteral("unknown key DEADBEEF12345678")),
                 QString{});
        QVERIFY(SigningKeyRecovery::containsUnknownKeyReport(
            QStringLiteral("unknown key DEADBEEF12345678")));
        const QString pacmanOutput = unknownKey(
            QStringLiteral("fluffnet"), QStringLiteral(
                "D22EA9EEF63938678070E27DBD8F8D65E5D7DA6F"));
        QCOMPARE(SigningKeyRecovery::repositoryName(pacmanOutput),
                 QStringLiteral("fluffnet"));
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(pacmanOutput),
                 QStringLiteral(
                     "D22EA9EEF63938678070E27DBD8F8D65E5D7DA6F"));
        QVERIFY(!SigningKeyRecovery::containsUnknownKeyReport(
            QStringLiteral("error: failed retrieving file")));
    }

    void rejectsOversizedFingerprintTokens()
    {
        const QString fortyOne(41, QLatin1Char('A'));
        const QString sixtyFour(64, QLatin1Char('B'));
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(
                     QStringLiteral("error: key \"%1\" is unknown").arg(fortyOne)),
                 QString{});
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(
                     QStringLiteral("unknown signing key %1").arg(sixtyFour)),
                 QString{});
    }

    void recognizesExactPacmanSigningKeyPrompts()
    {
        const QString fingerprint = QStringLiteral(
            "D22EA9EEF63938678070E27DBD8F8D65E5D7DA6F");
        const QString quoted = QStringLiteral(
            ":: Import PGP key \"%1\"? [Y/n]").arg(fingerprint);
        const QString unquoted = QStringLiteral(
            ":: Import PGP key %1? [Y/n]").arg(fingerprint);
        const QString keyIdWithUid = QStringLiteral(
            ":: Import PGP key BD8F8D65E5D7DA6F, "
            "\"Fluff Linux Repository Signing Key "
            "<flufflinux@fluffnet.org>\"? [Y/n]");
        QCOMPARE(SigningKeyRecovery::signingKeyImportPromptEnd(quoted),
                 quoted.size());
        QCOMPARE(SigningKeyRecovery::signingKeyImportPromptEnd(unquoted),
                 unquoted.size());
        QCOMPARE(SigningKeyRecovery::signingKeyImportPromptEnd(keyIdWithUid),
                 keyIdWithUid.size());
        QCOMPARE(SigningKeyRecovery::signingKeyImportPromptEnd(
                     QStringLiteral(":: Import PGP key \"%1? [Y/n]")
                         .arg(fingerprint)),
                 qsizetype(-1));
        QCOMPARE(SigningKeyRecovery::signingKeyImportPromptEnd(
                     QStringLiteral(":: Import PGP key \"%1\"? [y/N]")
                         .arg(fingerprint)),
                 qsizetype(-1));
    }

    void parsesPhotographedPacmanSyncFailure()
    {
        const QString fingerprint = QStringLiteral(
            "D22EA9EEF63938678070E27DBD8F8D65E5D7DA6F");
        const QString output = pacmanSyncKeyserverFailure(
            QStringLiteral("fluffnet"), fingerprint);
        QVERIFY(SigningKeyRecovery::containsUnknownKeyReport(output));
        QCOMPARE(SigningKeyRecovery::repositoryName(output),
                 QStringLiteral("fluffnet"));
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(output),
                 fingerprint);

        const QString remoteOnly = QStringLiteral(
            "error: key \"%1\" could not be looked up remotely\n"
            "error: failed to synchronize all databases (unexpected error)\n")
                                       .arg(fingerprint);
        QVERIFY(SigningKeyRecovery::containsUnknownKeyReport(remoteOnly));
        QCOMPARE(SigningKeyRecovery::repositoryName(remoteOnly), QString{});
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(remoteOnly),
                 fingerprint);

        int fetches = 0;
        SigningKeyRecovery unidentified(
            testConfig(),
            [&fetches](QString *) { ++fetches; return QByteArray{}; },
            [&fetches](QString *) { ++fetches; return QByteArray{}; },
            [](const QString &, const QStringList &) { return completed(); });
        const auto result = unidentified.recover(remoteOnly);
        QCOMPARE(result.category, QStringLiteral("repository-unidentified"));
        QCOMPARE(fetches, 0);
    }

    void rejectsAmbiguousRepositoryKeyReports()
    {
        const QString first = QStringLiteral(
            "D22EA9EEF63938678070E27DBD8F8D65E5D7DA6F");
        const QString second = QStringLiteral(
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        const QString output = pacmanSyncKeyserverFailure(
                                   QStringLiteral("fluffnet"), first)
            + unknownKey(QStringLiteral("core"), second);
        QCOMPARE(SigningKeyRecovery::repositoryName(output), QString{});
        QCOMPARE(SigningKeyRecovery::requestedFingerprint(output), QString{});

        int fingerprintFetches = 0;
        int certificateFetches = 0;
        int commands = 0;
        SigningKeyRecovery recovery(
            testConfig(),
            [&fingerprintFetches](QString *) {
                ++fingerprintFetches;
                return QByteArray{};
            },
            [&certificateFetches](QString *) {
                ++certificateFetches;
                return QByteArray{};
            },
            [&commands](const QString &, const QStringList &) {
                ++commands;
                return completed();
            });
        const auto result = recovery.recover(output);
        QCOMPARE(result.category, QStringLiteral("repository-unidentified"));
        QCOMPARE(fingerprintFetches, 0);
        QCOMPARE(certificateFetches, 0);
        QCOMPARE(commands, 0);
    }

    void failedTrustRollsBackNewImport()
    {
        QStringList operations;
        const QString sourceHome = m_keys.path();
        const QString primary = m_expectedPrimary;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [&operations, primary, sourceHome](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    if (operation == QStringLiteral("--finger")) {
                        return arguments.value(1) == primary
                            ? missingPublicKey() : completed();
                    }
                    return completed(operation
                                             == QStringLiteral("--lsign-key")
                                         ? 1 : 0);
                }
                return execute(program, arguments);
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QCOMPARE(result.category, QStringLiteral("key-trust-failed"));
        QVERIFY(operations.contains(QStringLiteral("--add")));
        QVERIFY(operations.contains(QStringLiteral("--delete")));
    }

    void temporaryKeyringIsRemoved()
    {
        QString temporaryHome;
        QStringList operations;
        const QString sourceHome = m_keys.path();
        const QString primary = m_expectedPrimary;
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [&temporaryHome, &operations, primary, sourceHome](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                const int homeIndex = arguments.indexOf(QStringLiteral("--homedir"));
                if (homeIndex >= 0
                    && arguments.value(homeIndex + 1) != sourceHome) {
                    temporaryHome = arguments.value(homeIndex + 1);
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    operations << arguments.value(0);
                    if (arguments.value(0) == QStringLiteral("--finger")
                        && arguments.value(1) == primary) {
                        return missingPublicKey();
                    }
                    return completed();
                }
                return execute(program, arguments);
            });
        const auto result = recovery.recover(
            unknownKey(QStringLiteral("fluffnet"), m_expectedSubkeys[0]));
        QVERIFY2(result.recovered, qPrintable(result.category));
        QVERIFY(!temporaryHome.isEmpty());
        QVERIFY(!QDir(temporaryHome).exists());
    }

    void concurrentLockIsExclusive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.path() + QStringLiteral("/recovery.lock");
        QLockFile first(path);
        QLockFile second(path);
        first.setStaleLockTime(0);
        second.setStaleLockTime(0);
        QVERIFY(first.tryLock(0));
        QVERIFY(!second.tryLock(0));
    }

    void issueDiagnosticsAreWhitelistedAndSanitized()
    {
        SigningKeyIssueDetails details;
        details.repository = QStringLiteral("fluffnet");
        details.fluVersion = QStringLiteral("1.4.0");
        details.osVersion = QStringLiteral(
            "Fluff Linux /home/alice/private token=supersecret");
        details.expectedFingerprint = m_expectedPrimary;
        details.receivedFingerprint = m_unrelatedPrimary;
        details.requestedFingerprint = m_expectedSubkeys[0];
        details.failureCategory = QStringLiteral("primary-fingerprint-mismatch");
        details.pacmanExitStatus = 1;
        const QUrl url = signingKeyIssueUrl(details);
        QCOMPARE(url.host(), QStringLiteral("github.com"));
        QCOMPARE(url.path(),
                 QStringLiteral("/FluffNet/flufflinux-update/issues/new"));
        const QString body =
            QUrlQuery(url).queryItemValue(QStringLiteral("body"));
        QVERIFY(body.contains(QStringLiteral("Repository: fluffnet")));
        QVERIFY(!body.contains(QStringLiteral("alice")));
        QVERIFY(!body.contains(QStringLiteral("supersecret")));
        QVERIFY(!body.contains(QStringLiteral("/home/")));
        QVERIFY(body.contains(m_expectedPrimary));
        QVERIFY(body.contains(m_expectedSubkeys[0]));

        int opened = 0;
        QVERIFY(openSigningKeyIssueUrl(url, [&opened](const QUrl &) {
            ++opened;
            return true;
        }));
        QCOMPARE(opened, 1);
        QVERIFY(!openSigningKeyIssueUrl(
            QUrl(QStringLiteral("https://example.invalid/issues/new")),
            [&opened](const QUrl &) {
                ++opened;
                return true;
            }));
        QCOMPARE(opened, 1);
    }

private:
    SigningKeyRecoveryConfig testConfig() const
    {
        return {};
    }

    SigningKeyRecovery makeRecovery(const QString &expectedPrimary,
                                    const QString &certificatePrimary,
                                    QStringList *pacmanOperations)
    {
        const QString sourceHome = m_keys.path();
        const auto certificateAdded = std::make_shared<bool>(false);
        return SigningKeyRecovery(
            testConfig(),
            [expectedPrimary](QString *) {
                return expectedPrimary.toLatin1();
            },
            [sourceHome, certificatePrimary](QString *) {
                return exportedCertificate(sourceHome, certificatePrimary);
            },
            [pacmanOperations, certificateAdded, certificatePrimary,
             sourceHome](
                const QString &program, const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-conf"))) {
                    return completedWithOutput(sourceHome.toLocal8Bit() + '\n');
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    pacmanOperations->append(operation);
                    if (operation == QStringLiteral("--finger")
                        && arguments.value(1) == certificatePrimary
                        && !*certificateAdded) {
                        return missingPublicKey();
                    }
                    if (operation == QStringLiteral("--add")) {
                        *certificateAdded = true;
                    }
                    return completed();
                }
                return execute(program, arguments);
            });
    }

    QTemporaryDir m_keys;
    QString m_expectedPrimary;
    QStringList m_expectedSubkeys;
    QString m_unrelatedPrimary;
    QString m_unrelatedSubkey;
};

QTEST_GUILESS_MAIN(SigningKeyRecoveryTest)

#include "signingkeyrecoverytest.moc"
