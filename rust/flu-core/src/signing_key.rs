//! Repository-scoped recovery of the exact signing material requested by Pacman.
//!
//! The desktop adapter supplies HTTPS downloads and bounded process execution.
//! All decisions, certificate checks, temporary files and rollback rules live
//! here, so the panel, helper and worker use the same implementation.

use regex::Regex;
use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::sync::LazyLock;

#[derive(Clone, Debug)]
pub struct Config {
    pub gpg_path: String,
    pub pacman_key_path: String,
    pub pacman_conf_path: String,
    pub maximum_fingerprint_response_bytes: i64,
    pub maximum_certificate_response_bytes: i64,
    pub maximum_pacman_conf_response_bytes: i64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            gpg_path: "/usr/bin/gpg".into(),
            pacman_key_path: "/usr/bin/pacman-key".into(),
            pacman_conf_path: "/usr/bin/pacman-conf".into(),
            maximum_fingerprint_response_bytes: 4096,
            maximum_certificate_response_bytes: 256 * 1024,
            maximum_pacman_conf_response_bytes: 4096,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct CommandResult {
    pub started: bool,
    pub finished: bool,
    pub exit_code: i32,
    pub output: Vec<u8>,
}

impl CommandResult {
    fn succeeded(&self) -> bool {
        self.started && self.finished && self.exit_code == 0
    }

    fn key_definitely_missing(&self) -> bool {
        if !self.started || !self.finished || self.exit_code == 0 {
            return false;
        }
        let output = String::from_utf8_lossy(&self.output).to_ascii_lowercase();
        output.contains("no public key")
            || output.contains("key not found")
            || (output.contains("the key identified by")
                && output.contains("could not be found locally"))
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RecoveryResult {
    pub recovered: bool,
    pub repository: String,
    pub category: String,
    pub expected_fingerprint: String,
    pub received_fingerprint: String,
    pub requested_fingerprint: String,
}

pub trait Context {
    fn fingerprint(&self) -> Result<Vec<u8>, String>;
    fn certificate(&self) -> Result<Vec<u8>, String>;
    fn run(&self, program: &str, arguments: &[String]) -> CommandResult;
}

static SIMPLE_ANSI: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\x1b\[[0-9;]*[A-Za-z]").unwrap());
static CSI_ANSI: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\x1b\[[0-?]*[ -/]*[@-~]").unwrap());
static KEY_WORD: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?i)(?:unknown[ -]key|signing[ -]key|key)").unwrap());
static HEX_TOKEN: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?i)[0-9a-f]{40,}").unwrap());
static REPOSITORY_REPORT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r#"(?im)(?:^|[\r\n])\s*error:\s*([A-Za-z0-9@._+:-]+):\s*key\s*["']?([0-9a-f]{40})["']?\s+is unknown(?:\s|$)"#,
    )
    .unwrap()
});
static REMOTE_FAILURE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r#"(?i)(?:^|[\r\n])\s*error:\s*key\s*["']?[0-9a-f]{40}["']?\s+could not be looked up remotely(?:\s|$)"#,
    )
    .unwrap()
});
static IMPORT_PROMPT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r#"Import\s+PGP\s+key\s+(["']?)(?:[0-9A-Fa-f]{40}|[0-9A-Fa-f]{16})(["']?)(?:,[^\r\n]{0,512})?\?\s*\[Y/n\]"#,
    )
    .unwrap()
});

pub fn normalize_fingerprint(response: &[u8]) -> String {
    let response = response.strip_suffix(b"\n").unwrap_or(response);
    if response.len() == 40
        && response
            .iter()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'A'..=b'F'))
    {
        String::from_utf8(response.to_vec()).unwrap()
    } else {
        String::new()
    }
}

fn record_fingerprint(value: &str) -> String {
    let value = value.trim().to_ascii_uppercase();
    if value.len() == 40 && value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        value
    } else {
        String::new()
    }
}

pub fn requested_fingerprint(output: &str) -> String {
    let plain = SIMPLE_ANSI.replace_all(output, "");
    let mut candidates = BTreeSet::new();
    for line in plain.split(['\r', '\n']) {
        for word in KEY_WORD.find_iter(line) {
            // Scan complete hex runs instead of accepting 40 characters from
            // a longer token. Rust regexes do not support PCRE lookarounds.
            if let Some(token) = HEX_TOKEN
                .find_iter(&line[word.end()..])
                .find(|token| token.as_str().len() == 40)
            {
                candidates.insert(token.as_str().to_ascii_uppercase());
            }
        }
    }
    if candidates.len() == 1 {
        candidates.into_iter().next().unwrap()
    } else {
        String::new()
    }
}

pub fn repository_name(output: &str) -> String {
    let plain = CSI_ANSI.replace_all(output, "");
    let reports: BTreeSet<_> = REPOSITORY_REPORT
        .captures_iter(&plain)
        .map(|capture| {
            (
                capture[1].to_ascii_lowercase(),
                capture[2].to_ascii_uppercase(),
            )
        })
        .collect();
    if reports.len() == 1 {
        reports.into_iter().next().unwrap().0
    } else {
        String::new()
    }
}

/// Return a UTF-16 position, matching QString's indexing contract.
pub fn signing_key_import_prompt_end(output: &str) -> i64 {
    for capture in IMPORT_PROMPT.captures_iter(output) {
        if capture[1] == capture[2] {
            return output[..capture.get(0).unwrap().end()]
                .encode_utf16()
                .count() as i64;
        }
    }
    -1
}

pub fn contains_unknown_key_report(output: &str) -> bool {
    let plain = SIMPLE_ANSI.replace_all(output, "");
    let lower = plain.to_ascii_lowercase();
    lower.contains("unknown signing key")
        || lower.contains("unknown key")
        || REPOSITORY_REPORT.is_match(&plain)
        || lower.contains("required key missing from keyring")
        || REMOTE_FAILURE.is_match(&plain)
        || signing_key_import_prompt_end(&plain) >= 0
}

#[derive(Default)]
struct Certificate {
    primary: String,
    primary_usable: bool,
    primary_signing_capable: bool,
    all_subkeys: BTreeSet<String>,
    usable_signing_subkeys: BTreeSet<String>,
}

fn describe_certificate(listing: &[u8]) -> Certificate {
    let listing = String::from_utf8_lossy(listing);
    let mut certificate = Certificate::default();
    let mut pending = "";
    let mut usable = false;
    let mut signing = false;
    for line in listing.split('\n') {
        let fields: Vec<_> = line.split(':').collect();
        if matches!(fields[0], "pub" | "sub") {
            pending = fields[0];
            let validity = fields.get(1).copied().unwrap_or("");
            let capabilities = fields.get(11).copied().unwrap_or("");
            usable = !matches!(validity.as_bytes().first(), Some(b'r' | b'e' | b'd' | b'i'))
                && !capabilities.contains('D');
            signing = capabilities.contains('s');
            continue;
        }
        if fields[0] != "fpr" || fields.len() <= 9 || pending.is_empty() {
            continue;
        }
        let fingerprint = record_fingerprint(fields[9]);
        if !fingerprint.is_empty() {
            if pending == "pub" && certificate.primary.is_empty() {
                certificate.primary = fingerprint;
                certificate.primary_usable = usable;
                certificate.primary_signing_capable = signing;
            } else if pending == "sub" {
                certificate.all_subkeys.insert(fingerprint.clone());
                if usable && signing {
                    certificate.usable_signing_subkeys.insert(fingerprint);
                }
            }
        }
        pending = "";
    }
    certificate
}

fn has_valid_subkey_binding(listing: &[u8], primary: &str, requested: &str) -> bool {
    let listing = String::from_utf8_lossy(listing);
    let mut current = String::new();
    let mut awaiting = false;
    for line in listing.split('\n') {
        let fields: Vec<_> = line.split(':').collect();
        if fields[0] == "sub" {
            awaiting = true;
            current.clear();
            continue;
        }
        if awaiting && fields[0] == "fpr" && fields.len() > 9 {
            current = record_fingerprint(fields[9]);
            awaiting = false;
            continue;
        }
        if fields.len() > 12
            && fields[0] == "sig"
            && fields[1] == "!"
            && current == requested
            && record_fingerprint(fields[12]) == primary
            && fields[10].to_ascii_lowercase().starts_with("18")
        {
            return true;
        }
    }
    false
}

fn has_valid_primary_self_signature(listing: &[u8], primary: &str) -> bool {
    String::from_utf8_lossy(listing).split('\n').any(|line| {
        let fields: Vec<_> = line.split(':').collect();
        fields.len() > 12
            && fields[0] == "sig"
            && fields[1] == "!"
            && record_fingerprint(fields[12]) == primary
            && matches!(
                fields[10]
                    .get(..2)
                    .unwrap_or("")
                    .to_ascii_lowercase()
                    .as_str(),
                "10" | "11" | "12" | "13" | "1f"
            )
    })
}

fn certificate_supports_request(
    listing: &[u8],
    checked: &[u8],
    primary: &str,
    requested: &str,
) -> bool {
    let certificate = describe_certificate(listing);
    certificate.primary == primary
        && certificate.primary_usable
        && has_valid_primary_self_signature(checked, primary)
        && if requested == primary {
            certificate.primary_signing_capable
        } else {
            certificate.all_subkeys.contains(requested)
                && certificate.usable_signing_subkeys.contains(requested)
                && has_valid_subkey_binding(checked, primary, requested)
        }
}

fn pacman_gpg_directory(result: &CommandResult, maximum: i64) -> Option<PathBuf> {
    let mut path = result.output.as_slice();
    if !result.succeeded() || path.is_empty() || path.len() as i64 > maximum || path.contains(&0) {
        return None;
    }
    if let Some(trimmed) = path.strip_suffix(b"\n") {
        path = trimmed.strip_suffix(b"\r").unwrap_or(trimmed);
    }
    if path.is_empty() || path.contains(&b'\r') || path.contains(&b'\n') {
        return None;
    }
    let decoded = String::from_utf8_lossy(path);
    let source = Path::new(decoded.as_ref());
    if !source.is_absolute() {
        return None;
    }
    let mut clean = PathBuf::from("/");
    for component in source.components() {
        match component {
            Component::Normal(part) => clean.push(part),
            Component::ParentDir => {
                clean.pop();
            }
            _ => {}
        }
    }
    (clean != Path::new("/") && clean.is_dir()).then_some(clean)
}

fn args(common: &[String], more: &[&str]) -> Vec<String> {
    common
        .iter()
        .cloned()
        .chain(more.iter().map(|s| (*s).into()))
        .collect()
}

pub fn recover(context: &impl Context, config: &Config, output: &str) -> RecoveryResult {
    let mut result = RecoveryResult {
        repository: repository_name(output),
        ..Default::default()
    };
    macro_rules! fail {
        ($category:expr) => {{
            result.category = $category.into();
            return result;
        }};
    }
    if result.repository.is_empty() {
        fail!("repository-unidentified");
    }
    if result.repository != "fluffnet" {
        fail!("repository-not-fluffnet");
    }
    result.requested_fingerprint = requested_fingerprint(output);
    if result.requested_fingerprint.is_empty() {
        fail!("requested-fingerprint-invalid");
    }

    let fingerprint = match context.fingerprint() {
        Ok(value) => value,
        Err(category) => {
            fail!(category);
        }
    };
    if fingerprint.len() as i64 > config.maximum_fingerprint_response_bytes {
        fail!("fingerprint-response-oversized");
    }
    result.expected_fingerprint = normalize_fingerprint(&fingerprint);
    if result.expected_fingerprint.is_empty() {
        fail!("fingerprint-response-malformed");
    }

    let response = match context.certificate() {
        Ok(value) => value,
        Err(category) => {
            fail!(category);
        }
    };
    if response.is_empty() {
        fail!("certificate-response-empty");
    }
    if response.len() as i64 > config.maximum_certificate_response_bytes {
        fail!("certificate-response-oversized");
    }

    let directory = match tempfile::Builder::new()
        .prefix("flufflinux-update-key-")
        .tempdir()
    {
        Ok(directory) => directory,
        Err(_) => {
            fail!("temporary-keyring-failed");
        }
    };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if fs::set_permissions(directory.path(), fs::Permissions::from_mode(0o700)).is_err() {
            fail!("temporary-keyring-failed");
        }
    }
    let downloaded = directory.path().join("downloaded-certificate.asc");
    if fs::write(&downloaded, &response).is_err() {
        fail!("certificate-write-failed");
    }
    let home = directory.path().to_string_lossy().into_owned();
    let downloaded = downloaded.to_string_lossy().into_owned();
    let common: Vec<String> = [
        "--batch",
        "--no-tty",
        "--no-auto-key-retrieve",
        "--no-auto-key-import",
        "--require-cross-certification",
        "--homedir",
        &home,
    ]
    .iter()
    .map(|s| (*s).into())
    .collect();

    let packets = context.run(
        &config.gpg_path,
        &args(&common, &["--list-packets", &downloaded]),
    );
    if !packets.succeeded() {
        fail!("certificate-parse-failed");
    }
    let lower = String::from_utf8_lossy(&packets.output).to_ascii_lowercase();
    if lower.contains("secret key packet") || lower.contains("secret sub key packet") {
        fail!("certificate-contains-private-key");
    }
    if !context
        .run(&config.gpg_path, &args(&common, &["--import", &downloaded]))
        .succeeded()
    {
        fail!("certificate-import-failed");
    }
    let listing = context.run(
        &config.gpg_path,
        &args(
            &common,
            &[
                "--with-colons",
                "--fixed-list-mode",
                "--fingerprint",
                "--fingerprint",
                "--list-keys",
                &result.requested_fingerprint,
            ],
        ),
    );
    if !listing.succeeded() {
        fail!("certificate-inspection-failed");
    }
    let certificate = describe_certificate(&listing.output);
    result.received_fingerprint = certificate.primary.clone();
    if certificate.primary.is_empty() {
        fail!("certificate-fingerprint-invalid");
    }
    if certificate.primary != result.expected_fingerprint {
        fail!("primary-fingerprint-mismatch");
    }
    if !certificate.primary_usable {
        fail!("primary-key-unusable");
    }
    let requested_primary = result.requested_fingerprint == certificate.primary;
    let requested_subkey = certificate
        .all_subkeys
        .contains(&result.requested_fingerprint);
    if !requested_primary && !requested_subkey {
        fail!("requested-key-not-in-certificate");
    }
    if (requested_subkey
        && !certificate
            .usable_signing_subkeys
            .contains(&result.requested_fingerprint))
        || (requested_primary && !certificate.primary_signing_capable)
    {
        fail!("signing-key-unusable");
    }

    let checked = context.run(
        &config.gpg_path,
        &args(
            &common,
            &[
                "--with-colons",
                "--no-sig-cache",
                "--fingerprint",
                "--fingerprint",
                "--check-sigs",
                &certificate.primary,
            ],
        ),
    );
    if !checked.succeeded()
        || !has_valid_primary_self_signature(&checked.output, &certificate.primary)
    {
        fail!("certificate-signature-invalid");
    }
    if requested_subkey
        && !has_valid_subkey_binding(
            &checked.output,
            &certificate.primary,
            &result.requested_fingerprint,
        )
    {
        fail!("subkey-binding-invalid");
    }
    let certificate_path = directory.path().join("certificate.gpg");
    let certificate_file = certificate_path.to_string_lossy().into_owned();
    if !context
        .run(
            &config.gpg_path,
            &args(
                &common,
                &[
                    "--output",
                    &certificate_file,
                    "--export",
                    &certificate.primary,
                ],
            ),
        )
        .succeeded()
        || !certificate_path.is_file()
    {
        fail!("certificate-export-failed");
    }

    let existing = context.run(
        &config.pacman_key_path,
        &args(&[], &["--finger", &certificate.primary]),
    );
    let was_missing = existing.key_definitely_missing();
    if !existing.succeeded() && !was_missing {
        fail!("keyring-inspection-failed");
    }
    let rollback = || {
        if was_missing {
            context.run(
                &config.pacman_key_path,
                &args(&[], &["--delete", &certificate.primary]),
            );
        }
    };
    if !context
        .run(
            &config.pacman_key_path,
            &args(&[], &["--add", &certificate_file]),
        )
        .succeeded()
    {
        rollback();
        fail!("key-import-failed");
    }
    let configured = context.run(&config.pacman_conf_path, &args(&[], &["gpgdir"]));
    let live_home =
        match pacman_gpg_directory(&configured, config.maximum_pacman_conf_response_bytes) {
            Some(path) => path.to_string_lossy().into_owned(),
            None => {
                rollback();
                fail!("keyring-inspection-failed");
            }
        };
    let live_common = args(
        &[],
        &[
            "--batch",
            "--no-tty",
            "--no-auto-check-trustdb",
            "--no-auto-key-retrieve",
            "--no-auto-key-import",
            "--require-cross-certification",
            "--homedir",
            &live_home,
        ],
    );
    let live_listing = context.run(
        &config.gpg_path,
        &args(
            &live_common,
            &[
                "--with-colons",
                "--fixed-list-mode",
                "--list-options",
                "show-unusable-subkeys",
                "--fingerprint",
                "--fingerprint",
                "--list-keys",
                &certificate.primary,
            ],
        ),
    );
    let live_checked = context.run(
        &config.gpg_path,
        &args(
            &live_common,
            &[
                "--with-colons",
                "--no-sig-cache",
                "--fingerprint",
                "--fingerprint",
                "--check-sigs",
                &certificate.primary,
            ],
        ),
    );
    if !live_listing.succeeded()
        || !live_checked.succeeded()
        || !certificate_supports_request(
            &live_listing.output,
            &live_checked.output,
            &certificate.primary,
            &result.requested_fingerprint,
        )
    {
        rollback();
        fail!("requested-key-import-failed");
    }
    if !context
        .run(
            &config.pacman_key_path,
            &args(&[], &["--lsign-key", &certificate.primary]),
        )
        .succeeded()
    {
        rollback();
        fail!("key-trust-failed");
    }
    result.recovered = true;
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    const A: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const B: &str = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";

    #[test]
    fn fingerprint_endpoint_keeps_strict_wire_format() {
        assert_eq!(normalize_fingerprint(A.as_bytes()), A);
        assert_eq!(normalize_fingerprint(format!("{A}\n").as_bytes()), A);
        for value in [
            format!(" {A}"),
            format!("{A}\r\n"),
            A.to_lowercase(),
            format!("{A}A"),
        ] {
            assert!(normalize_fingerprint(value.as_bytes()).is_empty());
        }
    }

    #[test]
    fn repository_attribution_is_unique_and_explicit() {
        let first = format!("error: fluffnet: key \"{A}\" is unknown\n");
        assert_eq!(repository_name(&first.repeat(2)), "fluffnet");
        assert_eq!(requested_fingerprint(&first.repeat(2)), A);
        assert!(
            repository_name(&format!("{first}error: core: key \"{B}\" is unknown\n")).is_empty()
        );
        assert!(
            repository_name(&format!("{first}error: core: key \"{A}\" is unknown\n")).is_empty()
        );
        assert!(
            repository_name(&format!(
                "error: key \"{A}\" could not be looked up remotely\n"
            ))
            .is_empty()
        );
        assert!(requested_fingerprint(&format!("unknown key {A}A")).is_empty());
        assert_eq!(
            requested_fingerprint(&format!("unknown key {A}A; signing key {B}")),
            B
        );
    }

    #[test]
    fn prompt_quotes_and_utf16_offsets_match_qstring() {
        for key in [A, "AAAAAAAAAAAAAAAA"] {
            for quote in ["", "\"", "'"] {
                let prompt = format!("שלום 🐧 :: Import PGP key {quote}{key}{quote}? [Y/n]");
                assert_eq!(
                    signing_key_import_prompt_end(&prompt),
                    prompt.encode_utf16().count() as i64
                );
            }
        }
        assert_eq!(
            signing_key_import_prompt_end(&format!("Import PGP key \"{A}? [Y/n]")),
            -1
        );
        assert_eq!(
            signing_key_import_prompt_end(&format!("Import PGP key {A}? [y/N]")),
            -1
        );
    }

    struct ForbiddenContext;
    impl Context for ForbiddenContext {
        fn fingerprint(&self) -> Result<Vec<u8>, String> {
            panic!("unexpected network access")
        }
        fn certificate(&self) -> Result<Vec<u8>, String> {
            panic!("unexpected network access")
        }
        fn run(&self, _: &str, _: &[String]) -> CommandResult {
            panic!("unexpected keyring access")
        }
    }

    #[test]
    fn foreign_and_unidentified_repositories_never_fetch_or_mutate() {
        for (output, category) in [
            (
                format!("error: core: key \"{A}\" is unknown"),
                "repository-not-fluffnet",
            ),
            (format!("unknown key {A}"), "repository-unidentified"),
            (
                format!(
                    "error: fluffnet: key \"{A}\" is unknown\nerror: core: key \"{B}\" is unknown"
                ),
                "repository-unidentified",
            ),
            (
                format!(
                    "error: fluffnet: key \"{A}\" is unknown\nerror: core: key \"{A}\" is unknown"
                ),
                "repository-unidentified",
            ),
        ] {
            assert_eq!(
                recover(&ForbiddenContext, &Config::default(), &output).category,
                category
            );
        }
    }
}
