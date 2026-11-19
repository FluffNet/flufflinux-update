//! Preserve explicitly unowned file conflicts before retrying a transaction.
use flu_core::runtime::{LOCK, PACMAN, arguments, run};
use regex::Regex;
use std::{
    collections::BTreeSet,
    fs::{self, File, OpenOptions},
    io::{self, Read},
    os::unix::{ffi::OsStrExt, fs::MetadataExt},
    path::{Component, Path, PathBuf},
    time::Duration,
};

// Hold Pacman's own lock while checking ownership and preserving files. Never
// delete an existing lock or let another transaction start between those steps.
struct DatabaseLock(File);
impl Drop for DatabaseLock {
    fn drop(&mut self) {
        if let (Ok(open), Ok(named)) = (self.0.metadata(), fs::symlink_metadata(LOCK)) {
            if open.dev() == named.dev() && open.ino() == named.ino() {
                let _ = fs::remove_file(LOCK);
            }
        }
    }
}

fn paths(output: &str) -> Result<BTreeSet<PathBuf>, String> {
    let pattern = Regex::new(
        r"(?m)^[^:\r\n]+:[ \t]+(/[^\r\n]*?) exists in filesystem(?: \(owned by [^\r\n]+\))?\r?$",
    )
    .unwrap();
    pattern
        .captures_iter(output)
        .map(|found| {
            let path = PathBuf::from(&found[1]);
            if path == Path::new("/")
                || path
                    .components()
                    .any(|part| matches!(part, Component::ParentDir))
            {
                Err(format!("unsafe conflict path: {}", path.display()))
            } else {
                Ok(path)
            }
        })
        .collect()
}

fn rename_without_replace(source: &Path, destination: &Path) -> io::Result<()> {
    let source = std::ffi::CString::new(source.as_os_str().as_bytes())?;
    let destination = std::ffi::CString::new(destination.as_os_str().as_bytes())?;
    // SAFETY: both C strings live for the syscall. No existing backup is replaced.
    #[cfg(target_os = "linux")]
    let status = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            source.as_ptr(),
            libc::AT_FDCWD,
            destination.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    #[cfg(target_os = "macos")]
    let status =
        unsafe { libc::renamex_np(source.as_ptr(), destination.as_ptr(), libc::RENAME_EXCL) };
    if status == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

pub fn recover(
    output: &str,
    recovered: &mut BTreeSet<PathBuf>,
) -> Result<Vec<(PathBuf, PathBuf)>, String> {
    let paths = paths(output)?;
    if paths.is_empty() {
        return Ok(Vec::new());
    }
    super::append_log(&format!(
        "\n[file recovery batch] {} conflicting paths\n",
        paths.len()
    ));
    let _lock = DatabaseLock(
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(LOCK)
            .map_err(|e| format!("cannot acquire database lock: {e}"))?,
    );
    let mut candidates = Vec::new();
    // Validate the entire batch before moving anything. In particular, a
    // package-owned file must not cause partial preservation of its neighbours.
    for path in paths {
        if recovered.contains(&path) {
            return Err(format!(
                "conflict returned after preservation: {}",
                path.display()
            ));
        }
        let metadata = fs::symlink_metadata(&path)
            .map_err(|e| format!("cannot inspect {}: {e}", path.display()))?;
        if !metadata.is_file() && !metadata.file_type().is_symlink() {
            return Err(format!(
                "not a regular file or symbolic link: {}",
                path.display()
            ));
        }
        let owner = run(
            PACMAN,
            &arguments(&["-Qo", "--", &path.to_string_lossy()]),
            Duration::from_secs(10),
            &[],
        );
        let diagnostic = String::from_utf8_lossy(&owner.output);
        let expected = format!("error: No package owns {}", path.display());
        if !owner.started
            || !owner.finished
            || owner.exit_code != 1
            || diagnostic.trim() != expected
        {
            return Err(format!(
                "ownership check rejected {}: started={}, finished={}, exit={}\n{}",
                path.display(),
                owner.started,
                owner.finished,
                owner.exit_code,
                diagnostic
            ));
        }
        candidates.push((path, metadata));
    }
    let mut backups = Vec::new();
    let mut random =
        File::open("/dev/urandom").map_err(|e| format!("cannot open random source: {e}"))?;
    for (path, checked) in candidates {
        let current = fs::symlink_metadata(&path)
            .map_err(|e| format!("cannot recheck {}: {e}", path.display()))?;
        if current.dev() != checked.dev() || current.ino() != checked.ino() {
            return Err(format!("file changed during recovery: {}", path.display()));
        }
        let mut backup = None;
        for _ in 0..100 {
            let mut bytes = [0_u8; 4];
            random
                .read_exact(&mut bytes)
                .map_err(|e| format!("cannot read random source: {e}"))?;
            let suffix = 10000 + u32::from_ne_bytes(bytes) % 90000;
            let destination = PathBuf::from(format!("{}.preupdate{suffix}", path.display()));
            match rename_without_replace(&path, &destination) {
                Ok(()) => {
                    backup = Some(destination);
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("cannot preserve {}: {e}", path.display())),
            }
        }
        let backup =
            backup.ok_or_else(|| format!("cannot allocate backup name: {}", path.display()))?;
        super::append_log(&format!(
            "\n[file conflict recovered]\n{} -> {}\n",
            path.display(),
            backup.display()
        ));
        recovered.insert(path.clone());
        backups.push((path, backup));
    }
    Ok(backups)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_all_conflicts_once_without_consuming_other_lines() {
        let output = "error: failed to commit transaction (conflicting files)\npkg: /one exists in filesystem (owned by owner)\npkg: /two with spaces exists in filesystem\npkg: /one exists in filesystem\nErrors occurred, no packages were upgraded.\n";
        assert_eq!(
            paths(output).unwrap(),
            BTreeSet::from([PathBuf::from("/one"), PathBuf::from("/two with spaces")])
        );
        assert!(paths("pkg: /one/../two exists in filesystem").is_err());
        assert!(paths("pkg: / exists in filesystem").is_err());
        assert!(
            paths("error:\n/one exists in filesystem")
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn backup_never_overwrites_existing_file() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        let backup = temp.path().join("backup");
        fs::write(&source, b"original").unwrap();
        fs::write(&backup, b"existing backup").unwrap();
        assert_eq!(
            rename_without_replace(&source, &backup).unwrap_err().kind(),
            io::ErrorKind::AlreadyExists
        );
        assert_eq!(fs::read(source).unwrap(), b"original");
        assert_eq!(fs::read(backup).unwrap(), b"existing backup");
    }
}
