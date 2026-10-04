//! Pacman's LC_ALL=C failure diagnostics, shared by the checker and worker.
//! Classify failed commands (including Pacman's exit-zero database errors).
//! Generic transaction/download wrappers do not
//! establish a network cause and must not enable conflict/removal recovery.
use crate::signing_key;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Failure {
    Network,
    Repository,
    Storage,
    Signature,
    Database,
    Archive,
    Filesystem,
    Locked,
}

// Also sent to the session notifier's translation table.
pub const MESSAGES: &[&str] = &[
    "No internet connection. Check your network and try again.",
    "The repository servers could not be reached. Try again later or check your mirror configuration.",
    "There is not enough free storage to continue the update. Free up space and try again.",
    "A package or repository signature could not be verified. The update was stopped. Check your system date and keyring, then try again.",
    "The package database is damaged or unreadable. The update was stopped. Please seek support before repairing the database.",
    "A downloaded package is damaged or incomplete. The update was stopped. Please try again or use a different mirror.",
    "The update could not read or write required files. Check filesystem permissions and disk health, then try again.",
    "The package database could not be locked. Close other package managers and check that the filesystem is writable, then try again.",
    "The updates could not be downloaded. Please try again or seek support.",
    "The update transaction could not be prepared. Please try again or seek support.",
    "The update could not start because sleep prevention could not be enabled. Please try again.",
];

impl Failure {
    pub fn code(self) -> &'static str {
        match self {
            Self::Network => "NETWORK_FAILED",
            Self::Repository => "REPOSITORY_UNAVAILABLE",
            Self::Storage => "INSUFFICIENT_STORAGE",
            Self::Signature => "SIGNATURE_INVALID",
            Self::Database => "DATABASE_INVALID",
            Self::Archive => "PACKAGE_CORRUPTED",
            Self::Filesystem => "FILESYSTEM_ERROR",
            Self::Locked => "DATABASE_LOCKED",
        }
    }
    pub fn message(self) -> &'static str {
        MESSAGES[self as usize]
    }
}

pub fn message(code: &str) -> &'static str {
    match code {
        "NETWORK_FAILED" => Failure::Network.message(),
        "REPOSITORY_UNAVAILABLE" => Failure::Repository.message(),
        "INSUFFICIENT_STORAGE" => Failure::Storage.message(),
        "SIGNATURE_INVALID" => Failure::Signature.message(),
        "DATABASE_INVALID" => Failure::Database.message(),
        "PACKAGE_CORRUPTED" => Failure::Archive.message(),
        "FILESYSTEM_ERROR" => Failure::Filesystem.message(),
        "DATABASE_LOCKED" => Failure::Locked.message(),
        "PACMAN_RUNNING" => "pacman process is already running.",
        "DOWNLOAD_FAILED" => MESSAGES[8],
        "TRANSACTION_PREPARE_FAILED" => MESSAGES[9],
        "SLEEP_INHIBITOR_FAILED" => MESSAGES[10],
        // Read state written by older workers without changing its meaning.
        "DOWNLOAD_CONNECTION_FAILED" => {
            "Connection failed while downloading updates. Check your network and try again."
        }
        "SIGNING_KEY_VERIFICATION_FAILED" => crate::desktop::SECURITY,
        "INSTALL_FAILED" => "The system update failed.",
        _ => "The update process could not be started.",
    }
}

pub fn classify(output: &str) -> Option<Failure> {
    let lower = output.to_ascii_lowercase();
    let has = |patterns: &[&str]| patterns.iter().any(|pattern| lower.contains(pattern));
    // The root cause wins over wrappers such as "failed retrieving file" or
    // "invalid or corrupted database (PGP signature)".
    Some(
        if has(&[
            "no space left on device",
            "not enough free disk space",
            "disk quota exceeded",
            "not enough space",
            "blocks needed",
        ]) {
            Failure::Storage
        } else if has(&[
            "read-only file system",
            "permission denied",
            "input/output error",
            "input/output failure",
            "failure writing output to destination",
            "failed writing received data",
        ]) {
            Failure::Filesystem
        } else if has(&["unable to lock database", "could not lock database"]) {
            Failure::Locked
        } else if has(&[
            "(pgp signature)",
            "invalid pgp signature",
            "missing pgp signature",
            "missing required signature",
            "signature check failed",
            "signature format error",
            "unsupported signature format",
            "signature is not valid",
            "signature is marginal trust",
            "signature is unknown trust",
            "required key missing from keyring",
        ]) || lower.lines().any(|line| {
            line.contains("signature from ")
                && (line.contains("unknown trust")
                    || line.contains("marginal trust")
                    || line.contains(" is invalid")
                    || line.contains(" has expired"))
        }) || signing_key::contains_unknown_key_report(output)
        {
            Failure::Signature
        } else if database_unusable(output)
            || has(&[
                "invalid or corrupted database",
                "could not open database",
                "could not create database",
                "could not find database",
                "database not initialized",
                "database is incorrect version",
                "database is inconsistent",
                "corrupted database entry",
                "duplicated database entry",
                "error parsing database file",
                "invalid name for database entry",
            ])
            || lower.lines().any(|line| {
                line.contains("could not be loaded into") && line.contains("sync database")
            })
        {
            Failure::Database
        } else if has(&[
            "invalid or corrupted package",
            "unrecognized archive format",
            "truncated input file",
            "damaged tar archive",
            "unexpected end of file",
        ]) {
            Failure::Archive
        } else if has(&[
            "could not resolve host",
            "name or service not known",
            "temporary failure in name resolution",
            "network is unreachable",
            "failed to connect",
            "connection timed out",
            "resolving timed out",
            "connection reset by peer",
            "operation too slow",
            "ssl connection timeout",
            "recv failure",
            "timeout was reached",
        ]) {
            Failure::Network
        } else if has(&[
            "failed retrieving file",
            "the requested url returned error",
            "too many errors from",
            "ssl certificate problem",
            "server certificate verification failed",
        ]) {
            Failure::Repository
        } else {
            return None;
        },
    )
}

/// Pacman can emit these errors while skipping an unreadable sync database,
/// then exit zero with "there is nothing to do". Never report that as success.
/// Do not apply this exception to transient mirror failures: Pacman may recover
/// from those by using a subsequent mirror within the same command.
pub fn database_unusable(output: &str) -> bool {
    output.lines().any(|line| {
        let line = line.to_ascii_lowercase();
        line.contains("error:")
            && ([
                "invalid or corrupted database",
                "database is inconsistent",
                "database is incorrect version",
                "corrupted database entry",
                "error parsing database file",
            ]
            .iter()
            .any(|s| line.contains(s))
                || (line.contains("could not open file")
                    && line.contains(".db:")
                    && (line.contains("unrecognized archive format")
                        || line.contains("truncated")
                        || line.contains("damaged"))))
    })
}

/// libcurl can hide ENOSPC behind CURLE_WRITE_ERROR. Only inspect the known
/// download destination after an explicit local-write failure; never infer a
/// network problem from its generic "failed retrieving file" wrapper.
pub fn classify_download(output: &str, cache: &std::path::Path) -> Option<Failure> {
    let failure = classify(output);
    let lower = output.to_ascii_lowercase();
    if failure != Some(Failure::Filesystem)
        || !(lower.contains("failure writing output to destination")
            || lower.contains("failed writing received data"))
    {
        return failure;
    }
    use std::os::unix::ffi::OsStrExt;
    let Ok(path) = std::ffi::CString::new(cache.as_os_str().as_bytes()) else {
        return failure;
    };
    let mut stats = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    // SAFETY: path is NUL-terminated and stats points to writable storage.
    if unsafe { libc::statvfs(path.as_ptr(), stats.as_mut_ptr()) } == 0 {
        // SAFETY: a successful statvfs initialized the structure.
        let stats = unsafe { stats.assume_init() };
        if stats.f_bavail == 0 || (stats.f_files > 0 && stats.f_favail == 0) {
            return Some(Failure::Storage);
        }
    }
    failure
}

/// An independent storage/I/O failure must not initiate key import.
/// Repository and fingerprint authorization remain in the existing verifier.
pub fn may_recover_key(output: &str) -> bool {
    signing_key::contains_unknown_key_report(output)
        && matches!(classify(output), None | Some(Failure::Signature))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn specific_causes_win_over_generic_pacman_wrappers() {
        for (cause, expected) in [
            (
                "error: failed retrieving file: No space left on device",
                Failure::Storage,
            ),
            (
                "error: Partition / too full: 100 blocks needed, 0 blocks free",
                Failure::Storage,
            ),
            (
                "error: failed retrieving file: Read-only file system",
                Failure::Filesystem,
            ),
            ("error: Permission denied", Failure::Filesystem),
            (
                "error: failed retrieving file: Failure writing output to destination",
                Failure::Filesystem,
            ),
            ("error: unable to lock database", Failure::Locked),
            (
                "error: invalid or corrupted database (PGP signature)",
                Failure::Signature,
            ),
            (
                "error: package missing required signature",
                Failure::Signature,
            ),
            (
                "error: signature from \"Arch Maintainer\" is unknown trust",
                Failure::Signature,
            ),
            ("error: invalid or corrupted database", Failure::Database),
            (
                "error: local database is inconsistent: name mismatch on package foo",
                Failure::Database,
            ),
            (
                "error: invalid or corrupted package (checksum)",
                Failure::Archive,
            ),
            (
                "error: failed retrieving file: Could not resolve host",
                Failure::Network,
            ),
            (
                "error: failed retrieving file: The requested URL returned error: 503",
                Failure::Repository,
            ),
        ] {
            let output = format!(
                "{cause}\nerror: failed to synchronize all databases\nerror: failed to commit transaction (unexpected error)"
            );
            assert_eq!(classify(&output), Some(expected), "{output}");
            assert_eq!(message(expected.code()), expected.message());
        }
    }

    #[test]
    fn generic_failures_and_conflicts_are_not_network_failures() {
        for output in [
            "error: failed to synchronize all databases (unexpected error)",
            "error: failed to prepare transaction (could not satisfy dependencies)\n:: installing libfoo breaks dependency 'libfoo=1' required by old-app",
            ":: foo and bar are in conflict. Remove bar? [y/N]",
            "foo: /etc/foo exists in filesystem",
            "checking keys in keyring...\nchecking package integrity...",
        ] {
            assert_eq!(classify(output), None, "{output}");
        }
        assert_ne!(message("DOWNLOAD_FAILED"), message("NETWORK_FAILED"));
        assert_ne!(
            message("TRANSACTION_PREPARE_FAILED"),
            message("NETWORK_FAILED")
        );
    }

    #[test]
    fn unreadable_database_is_not_a_successful_empty_update() {
        let output = "error: could not open file /var/lib/pacman/sync/extra.db: Unrecognized archive format\nthere is nothing to do";
        assert!(database_unusable(output));
        assert_eq!(classify(output), Some(Failure::Database));
        assert!(!database_unusable(
            "error: failed retrieving file 'extra.db': timeout\nextra is up to date"
        ));
    }

    #[test]
    fn unavailable_capacity_does_not_invent_a_full_disk() {
        let temp = tempfile::tempdir().unwrap();
        let missing = temp.path().join("absent");
        assert_eq!(
            classify_download("error: Failure writing output to destination", &missing),
            Some(Failure::Filesystem)
        );
        assert_eq!(
            classify_download("error: Could not resolve host", &missing),
            Some(Failure::Network)
        );
    }

    #[test]
    fn operational_failures_do_not_initiate_key_recovery() {
        let unknown =
            "error: fluffnet: key \"D22EA9EEF63938678707E27DBD8FB065E5D7DA6F\" is unknown";
        assert!(may_recover_key(unknown));
        assert!(!may_recover_key(&format!(
            "{unknown}\nerror: No space left on device"
        )));
        assert!(!may_recover_key(
            "error: invalid or corrupted package (PGP signature)"
        ));
    }
}
