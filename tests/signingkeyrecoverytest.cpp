#include "signingkeyrecovery.h"
#include "securitydiagnostics.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

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

SigningKeyCommandResult completed(int exitCode = 0)
{
    SigningKeyCommandResult result;
    result.started = true;
    result.finished = true;
    result.exitCode = exitCode;
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

    void acceptsPrimaryAndCertifiedRotatedSubkeys_data()
    {
        QTest::addColumn<QString>("requested");
        QTest::newRow("primary") << m_expectedPrimary;
        QTest::newRow("current-subkey") << m_expectedSubkeys[0];
        QTest::newRow("rotated-subkey") << m_expectedSubkeys[1];
    }

    void acceptsPrimaryAndCertifiedRotatedSubkeys()
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
        QTest::newRow("revoked") << QByteArray("r") << false;
        QTest::newRow("disabled") << QByteArray("d") << false;
        QTest::newRow("invalid") << QByteArray("i") << false;
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

    void failedTrustRollsBackNewImport()
    {
        QStringList operations;
        const QString sourceHome = m_keys.path();
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [&operations](const QString &program,
                          const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    const QString operation = arguments.value(0);
                    operations << operation;
                    return completed(operation == QStringLiteral("--lsign-key")
                                             || operation == QStringLiteral("--finger")
                                         ? 1
                                         : 0);
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
        SigningKeyRecovery recovery(
            testConfig(),
            [this](QString *) { return m_expectedPrimary.toLatin1(); },
            [sourceHome, this](QString *) {
                return exportedCertificate(sourceHome, m_expectedPrimary);
            },
            [&temporaryHome, &operations](const QString &program,
                                          const QStringList &arguments) {
                const int homeIndex = arguments.indexOf(QStringLiteral("--homedir"));
                if (homeIndex >= 0) {
                    temporaryHome = arguments.value(homeIndex + 1);
                }
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    operations << arguments.value(0);
                    return completed(arguments.value(0)
                                             == QStringLiteral("--finger")
                                         ? 1
                                         : 0);
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
        return SigningKeyRecovery(
            testConfig(),
            [expectedPrimary](QString *) {
                return expectedPrimary.toLatin1();
            },
            [sourceHome, certificatePrimary](QString *) {
                return exportedCertificate(sourceHome, certificatePrimary);
            },
            [pacmanOperations](const QString &program,
                               const QStringList &arguments) {
                if (program.endsWith(QStringLiteral("pacman-key"))) {
                    pacmanOperations->append(arguments.value(0));
                    return completed(arguments.value(0)
                                             == QStringLiteral("--finger")
                                         ? 1
                                         : 0);
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
