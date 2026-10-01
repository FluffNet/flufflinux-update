#include "signingkeyrecovery.h"
#include "signingkeycontext.h"
#include "flu_test_bridge/src/recovery.cxx.h"

#include <utility>

namespace
{
QString fromRust(rust::Str value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

rust::Vec<uint8_t> rustBytes(const QByteArray &bytes)
{
    rust::Vec<uint8_t> result;
    result.reserve(static_cast<size_t>(bytes.size()));
    for (const char byte : bytes) {
        result.push_back(static_cast<uint8_t>(byte));
    }
    return result;
}

flu::Artifact fetchArtifact(const SigningKeyRecovery::Fetcher &fetcher)
{
    QString failure;
    const QByteArray bytes = fetcher ? fetcher(&failure) : QByteArray{};
    flu::Artifact artifact;
    artifact.bytes = rustBytes(bytes);
    artifact.failure = failure.toStdString();
    return artifact;
}
}

namespace flu
{
SigningKeyContext::SigningKeyContext(
    const SigningKeyRecoveryConfig &configuration,
    const SigningKeyRecovery::Fetcher &fingerprintFetcher,
    const SigningKeyRecovery::Fetcher &certificateFetcher,
    const SigningKeyRecovery::Runner &runner)
    : m_config(configuration)
    , m_fingerprintFetcher(fingerprintFetcher)
    , m_certificateFetcher(certificateFetcher)
    , m_runner(runner)
{
}

Artifact SigningKeyContext::fingerprint() const
{
    return fetchArtifact(m_fingerprintFetcher);
}

Artifact SigningKeyContext::certificate() const
{
    return fetchArtifact(m_certificateFetcher);
}

CxxCommandResult SigningKeyContext::run(
    rust::Str program, const rust::Vec<rust::String> &arguments) const
{
    QStringList qtArguments;
    for (const auto &argument : arguments) {
        qtArguments.append(fromRust(argument));
    }
    const SigningKeyCommandResult value = m_runner
        ? m_runner(fromRust(program), qtArguments) : SigningKeyCommandResult{};
    CxxCommandResult result;
    result.started = value.started;
    result.finished = value.finished;
    result.exit_code = value.exitCode;
    result.output = rustBytes(value.output);
    return result;
}

RecoveryConfig SigningKeyContext::config() const
{
    RecoveryConfig result;
    result.gpg_path = m_config.gpgPath.toStdString();
    result.pacman_key_path = m_config.pacmanKeyPath.toStdString();
    result.pacman_conf_path = m_config.pacmanConfPath.toStdString();
    result.maximum_fingerprint_response_bytes =
        m_config.maximumFingerprintResponseBytes;
    result.maximum_certificate_response_bytes =
        m_config.maximumCertificateResponseBytes;
    result.maximum_pacman_conf_response_bytes =
        m_config.maximumPacmanConfResponseBytes;
    return result;
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
    return fromRust(flu::normalize_fingerprint(rust::Slice<const uint8_t>(
        reinterpret_cast<const uint8_t *>(value.constData()),
        static_cast<size_t>(value.size()))));
}

QString SigningKeyRecovery::requestedFingerprint(const QString &output)
{
    return fromRust(flu::requested_fingerprint(output.toStdString()));
}

QString SigningKeyRecovery::repositoryName(const QString &output)
{
    return fromRust(flu::repository_name(output.toStdString()));
}

bool SigningKeyRecovery::containsUnknownKeyReport(const QString &output)
{
    return flu::contains_unknown_key_report(output.toStdString());
}

qsizetype SigningKeyRecovery::signingKeyImportPromptEnd(const QString &output)
{
    return static_cast<qsizetype>(
        flu::signing_key_import_prompt_end(output.toStdString()));
}

SigningKeyRecoveryResult SigningKeyRecovery::recover(const QString &output) const
{
    const flu::SigningKeyContext context(
        m_config, m_fingerprintFetcher, m_certificateFetcher, m_runner);
    const auto value = flu::recover_signing_key(context, output.toStdString());
    SigningKeyRecoveryResult result;
    result.recovered = value.recovered;
    result.repository = fromRust(value.repository);
    result.category = fromRust(value.category);
    result.expectedFingerprint = fromRust(value.expected_fingerprint);
    result.receivedFingerprint = fromRust(value.received_fingerprint);
    result.requestedFingerprint = fromRust(value.requested_fingerprint);
    return result;
}
