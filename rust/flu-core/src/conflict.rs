//! Narrow package-conflict exception backed by Pacman's signature verification.
//! Never changes file ownership, dependency checks, or the installed database.
use crate::runtime::{PACMAN, arguments, run};
use regex::Regex;
use std::{
    fs,
    time::{Duration, Instant},
};

fn without_version(label: &str) -> Option<&str> {
    let mut fields = label.rsplitn(3, '-');
    let release = fields.next()?;
    let version = fields.next()?;
    let name = fields.next()?;
    (!release.is_empty() && !version.is_empty() && !name.is_empty()).then_some(name)
}

pub struct Conflict {
    pub incoming: String,
    pub version: Option<String>,
    pub removed: String,
    pub end: usize,
}

pub fn package_conflict(text: &str) -> Option<Conflict> {
    let pattern = Regex::new(r"(?m)(?:^|\n):: ([A-Za-z0-9@._+:-]+) and ([A-Za-z0-9@._+:-]+) are in conflict(?: \([^\n]*\))?\. Remove ([A-Za-z0-9@._+:-]+)\? \[y/N\]").unwrap();
    let found = pattern.captures(text)?;
    let matches_removed =
        |label: &str| label == &found[3] || without_version(label) == Some(&found[3]);
    if matches_removed(&found[1]) == matches_removed(&found[2]) {
        return None;
    }
    let incoming = if matches_removed(&found[2]) {
        &found[1]
    } else if matches_removed(&found[1]) {
        &found[2]
    } else {
        return None;
    };
    let versioned = found[1] != found[3] && found[2] != found[3];
    let (incoming, version) = if versioned {
        let name = without_version(incoming)?;
        (name, Some(incoming[name.len() + 1..].to_string()))
    } else {
        (incoming, None)
    };
    (incoming != &found[3]).then(|| Conflict {
        incoming: incoming.into(),
        version,
        removed: found[3].into(),
        end: found.get(0).unwrap().end(),
    })
}

fn identity(text: &str) -> Option<(&str, &str, &str)> {
    let mut lines = text.lines();
    let fields: Vec<_> = lines.next()?.split('|').collect();
    if lines.next().is_some()
        || fields.len() != 4
        || fields[0] != "fluffnet"
        || fields[1].is_empty()
        || fields[2].is_empty()
        || fields[3].len() != 64
        || !fields[3].bytes().all(|b| b.is_ascii_hexdigit())
    {
        return None;
    }
    Some((fields[1], fields[2], fields[3]))
}

fn eligible_metadata(text: &str, name: &str, version: &str) -> bool {
    let one = |key: &str| {
        let mut values = text.lines().filter_map(|line| line.strip_prefix(key));
        let first = values.next();
        if values.next().is_some() { None } else { first }
    };
    one("pkgname = ") == Some(name)
        && one("pkgver = ") == Some(version)
        && ((name.starts_with("flufflinux-") && name.len() > "flufflinux-".len())
            || one("packager = ") == Some("FluffNet LLC"))
}

/// Verify the exact candidate in an isolated, root-private Pacman database.
/// A user-writable checkupdates database is not a trust anchor. Refresh the
/// configured repository separately and require both database and package
/// signatures, even if the machine's ordinary SigLevel is weaker.
pub fn verified_fluffnet(database: &str, package: &str, expected_version: Option<&str>) -> bool {
    if package.is_empty()
        || !package
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"@._+:-".contains(&b))
    {
        return false;
    }
    let started = Instant::now();
    let execute = |program: &str, args: &[&str]| {
        let remaining = Duration::from_secs(90).checked_sub(started.elapsed())?;
        let result = run(
            program,
            &arguments(args),
            remaining.min(Duration::from_secs(60)),
            &[],
        );
        (result.started && result.finished && result.exit_code == 0)
            .then(|| String::from_utf8_lossy(&result.output).into_owned())
    };
    let lookup = |candidate: &str| {
        execute(
            PACMAN,
            &[
                "--dbpath",
                database,
                "-Spdd",
                "--print-format",
                "%r|%n|%v|%h",
                "--",
                candidate,
            ],
        )
    };
    let Some(selected) = lookup(package) else {
        return false;
    };
    let Some((name, version, _)) = identity(selected.trim()) else {
        eprintln!("[conflict verification] candidate identity invalid");
        return false;
    };
    if name != package || expected_version.is_some_and(|expected| expected != version) {
        return false;
    }
    let Some(repos) = execute("/usr/bin/pacman-conf", &["--repo-list"]) else {
        return false;
    };
    if !repos.lines().any(|repo| repo == "fluffnet") {
        eprintln!("[conflict verification] repository disabled");
        return false;
    }
    let Some(servers) = execute("/usr/bin/pacman-conf", &["--repo", "fluffnet", "Server"]) else {
        return false;
    };
    let servers: Vec<_> = servers.lines().filter(|line| !line.is_empty()).collect();
    if servers.is_empty()
        || servers.iter().any(|server| {
            !(server.starts_with("https://") || server.starts_with("http://"))
                || server.chars().any(char::is_whitespace)
        })
    {
        eprintln!("[conflict verification] unsupported server configuration");
        return false;
    }
    let Ok(temp) = tempfile::Builder::new()
        .prefix("flu-conflict-")
        .tempdir_in("/run")
    else {
        eprintln!("[conflict verification] cannot create private directory");
        return false;
    };
    let db = temp.path().join("db");
    let cache = temp.path().join("cache");
    if fs::create_dir(&db).is_err() || fs::create_dir(&cache).is_err() {
        return false;
    }
    let config = temp.path().join("pacman.conf");
    let config_text = format!(
        "[options]\nArchitecture = auto\nDBPath = {}\nCacheDir = {}\nGPGDir = /etc/pacman.d/gnupg\nLogFile = /dev/null\nSigLevel = Required TrustedOnly\n[fluffnet]\n{}\n",
        db.display(),
        cache.display(),
        servers
            .iter()
            .map(|server| format!("Server = {server}"))
            .collect::<Vec<_>>()
            .join("\n")
    );
    if fs::write(&config, config_text).is_err() {
        return false;
    }
    let config = config.to_string_lossy();
    if execute(PACMAN, &["--config", &config, "-Sy", "--noconfirm"]).is_none() {
        return false;
    }
    let target = format!("fluffnet/{name}");
    let Some(trusted) = execute(
        PACMAN,
        &[
            "--config",
            &config,
            "-Spdd",
            "--print-format",
            "%r|%n|%v|%h",
            "--",
            &target,
        ],
    ) else {
        return false;
    };
    if identity(trusted.trim()).is_none() || selected.trim() != trusted.trim() {
        eprintln!("[conflict verification] repository identity mismatch");
        return false;
    }
    // Download-only skips package conflicts without removing or installing
    // anything. Required signatures are checked before reading the archive.
    if execute(
        PACMAN,
        &["--config", &config, "-Swdd", "--noconfirm", "--", &target],
    )
    .is_none()
    {
        return false;
    }
    let Ok(entries) = fs::read_dir(&cache) else {
        return false;
    };
    let archives: Vec<_> = entries
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| {
            path.file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| {
                    name.contains(".pkg.tar.")
                        && !name.ends_with(".sig")
                        && !name.ends_with(".part")
                })
        })
        .collect();
    if archives.len() != 1 {
        return false;
    }
    execute(
        "/usr/bin/bsdtar",
        &["-xOf", &archives[0].to_string_lossy(), ".PKGINFO"],
    )
    .is_some_and(|metadata| eligible_metadata(&metadata, name, version))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_explicit_package_conflicts_identify_an_incoming_package() {
        let conflict = package_conflict(
            ":: flufflinux-remote and krdp are in conflict (remote). Remove krdp? [y/N]",
        )
        .unwrap();
        assert_eq!(conflict.incoming, "flufflinux-remote");
        assert_eq!(conflict.removed, "krdp");
        let versioned = package_conflict(
            ":: flufflinux-remote-2:1.0-3 and krdp-6.5-1 are in conflict. Remove krdp? [y/N]",
        )
        .unwrap();
        assert_eq!(versioned.incoming, "flufflinux-remote");
        assert_eq!(versioned.version.as_deref(), Some("2:1.0-3"));
        assert!(package_conflict("Remove krdp? [y/N]").is_none());
        assert!(package_conflict("foo: /usr/bin/bar exists in filesystem").is_none());
        assert!(package_conflict(":: foo and bar are in conflict. Remove baz? [y/N]").is_none());
        assert!(package_conflict(":: foo and bar are in conflict. Remove bar? [y/").is_none());
    }
    #[test]
    fn metadata_requires_exact_identity_and_prefix_or_exact_packager() {
        assert!(eligible_metadata(
            "pkgname = flufflinux-test\npkgver = 1-1\npackager = Someone",
            "flufflinux-test",
            "1-1"
        ));
        assert!(eligible_metadata(
            "pkgname = test\npkgver = 1-1\npackager = FluffNet LLC",
            "test",
            "1-1"
        ));
        for packager in [
            "Someone",
            "FluffNet LLC impersonator",
            "FluffNet LLC <fake@example.org>",
        ] {
            assert!(!eligible_metadata(
                &format!("pkgname = test\npkgver = 1-1\npackager = {packager}"),
                "test",
                "1-1"
            ));
        }
        assert!(!eligible_metadata(
            "pkgname = flufflinux-test\npkgver = 2-1",
            "flufflinux-test",
            "1-1"
        ));
        assert!(!eligible_metadata(
            "pkgname = flufflinux-test\npkgname = other\npkgver = 1-1",
            "flufflinux-test",
            "1-1"
        ));
    }
    #[test]
    fn repository_and_digest_are_mandatory_and_unambiguous() {
        let digest = "a".repeat(64);
        assert!(identity(&format!("fluffnet|test|1-1|{digest}")).is_some());
        for text in [
            format!("extra|test|1-1|{digest}"),
            "fluffnet|test|1-1|".into(),
            format!("fluffnet|test|1-1|{digest}\nother"),
        ] {
            assert!(identity(&text).is_none());
        }
    }
}
