#pragma once

#include "signingkeyrecovery.h"
#include "rust/cxx.h"

namespace flu
{
struct Artifact;
struct CxxCommandResult;
struct RecoveryConfig;

// Supplies only existing Qt I/O callbacks. Recovery decisions are in Rust.
class SigningKeyContext final
{
public:
    SigningKeyContext(const SigningKeyRecoveryConfig &configuration,
                      const SigningKeyRecovery::Fetcher &fingerprintFetcher,
                      const SigningKeyRecovery::Fetcher &certificateFetcher,
                      const SigningKeyRecovery::Runner &runner);

    Artifact fingerprint() const;
    Artifact certificate() const;
    CxxCommandResult run(rust::Str program,
                         const rust::Vec<rust::String> &arguments) const;
    RecoveryConfig config() const;

private:
    const SigningKeyRecoveryConfig &m_config;
    const SigningKeyRecovery::Fetcher &m_fingerprintFetcher;
    const SigningKeyRecovery::Fetcher &m_certificateFetcher;
    const SigningKeyRecovery::Runner &m_runner;
};
}
