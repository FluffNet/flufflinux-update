use flu_core::signing_key::{self, CommandResult, Config, Context};

#[cxx::bridge(namespace = "flu")]
mod ffi {
    struct Artifact {
        bytes: Vec<u8>,
        failure: String,
    }

    struct CxxCommandResult {
        started: bool,
        finished: bool,
        exit_code: i32,
        output: Vec<u8>,
    }

    struct RecoveryConfig {
        gpg_path: String,
        pacman_key_path: String,
        pacman_conf_path: String,
        maximum_fingerprint_response_bytes: i64,
        maximum_certificate_response_bytes: i64,
        maximum_pacman_conf_response_bytes: i64,
    }

    struct CxxRecoveryResult {
        recovered: bool,
        repository: String,
        category: String,
        expected_fingerprint: String,
        received_fingerprint: String,
        requested_fingerprint: String,
    }

    unsafe extern "C++" {
        include!("signingkeycontext.h");
        type SigningKeyContext;
        fn fingerprint(self: &SigningKeyContext) -> Artifact;
        fn certificate(self: &SigningKeyContext) -> Artifact;
        fn run(
            self: &SigningKeyContext,
            program: &str,
            arguments: &Vec<String>,
        ) -> CxxCommandResult;
        fn config(self: &SigningKeyContext) -> RecoveryConfig;
    }

    extern "Rust" {
        fn diagnostic_value(value: &str, maximum: usize) -> String;
        fn diagnostic_issue_url(fields: &[String], status: i32) -> String;
        fn diagnostic_issue_url_allowed(url: &str) -> bool;
        fn normalize_fingerprint(response: &[u8]) -> String;
        fn requested_fingerprint(output: &str) -> String;
        fn repository_name(output: &str) -> String;
        fn contains_unknown_key_report(output: &str) -> bool;
        fn signing_key_import_prompt_end(output: &str) -> i64;
        fn recover_signing_key(context: &SigningKeyContext, output: &str) -> CxxRecoveryResult;
    }
}

struct QtContext<'a>(&'a ffi::SigningKeyContext);

fn diagnostic_value(value: &str, maximum: usize) -> String {
    flu_core::desktop::sanitize(value, maximum)
}
fn diagnostic_issue_url(fields: &[String], status: i32) -> String {
    flu_core::desktop::issue_url(fields, status)
}
fn diagnostic_issue_url_allowed(url: &str) -> bool {
    flu_core::desktop::issue_url_allowed(url)
}

fn artifact_result(artifact: ffi::Artifact) -> Result<Vec<u8>, String> {
    if artifact.failure.is_empty() {
        Ok(artifact.bytes)
    } else {
        Err(artifact.failure)
    }
}

impl Context for QtContext<'_> {
    fn fingerprint(&self) -> Result<Vec<u8>, String> {
        artifact_result(self.0.fingerprint())
    }
    fn certificate(&self) -> Result<Vec<u8>, String> {
        artifact_result(self.0.certificate())
    }
    fn run(&self, program: &str, arguments: &[String]) -> CommandResult {
        let result = self.0.run(program, &arguments.to_vec());
        CommandResult {
            started: result.started,
            finished: result.finished,
            exit_code: result.exit_code,
            output: result.output,
        }
    }
}

fn normalize_fingerprint(response: &[u8]) -> String {
    signing_key::normalize_fingerprint(response)
}
fn requested_fingerprint(output: &str) -> String {
    signing_key::requested_fingerprint(output)
}
fn repository_name(output: &str) -> String {
    signing_key::repository_name(output)
}
fn contains_unknown_key_report(output: &str) -> bool {
    signing_key::contains_unknown_key_report(output)
}
fn signing_key_import_prompt_end(output: &str) -> i64 {
    signing_key::signing_key_import_prompt_end(output)
}

fn recover_signing_key(context: &ffi::SigningKeyContext, output: &str) -> ffi::CxxRecoveryResult {
    let value = context.config();
    let config = Config {
        gpg_path: value.gpg_path,
        pacman_key_path: value.pacman_key_path,
        pacman_conf_path: value.pacman_conf_path,
        maximum_fingerprint_response_bytes: value.maximum_fingerprint_response_bytes,
        maximum_certificate_response_bytes: value.maximum_certificate_response_bytes,
        maximum_pacman_conf_response_bytes: value.maximum_pacman_conf_response_bytes,
    };
    let result = signing_key::recover(&QtContext(context), &config, output);
    ffi::CxxRecoveryResult {
        recovered: result.recovered,
        repository: result.repository,
        category: result.category,
        expected_fingerprint: result.expected_fingerprint,
        received_fingerprint: result.received_fingerprint,
        requested_fingerprint: result.requested_fingerprint,
    }
}
