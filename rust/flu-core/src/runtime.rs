//! Native process, filesystem and HTTPS I/O shared by the Rust applications.
//! No test overrides are accepted by the production command-line programs.

use crate::signing_key::{self, CommandResult, Config, Context, RecoveryResult};
use fs2::FileExt;
use serde_json::Value;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

pub const PACMAN: &str = "/usr/bin/pacman";
pub const HELPER: &str = "/usr/lib/flufflinux-update/flufflinux-update-helper";
pub const STATE: &str = "/etc/pacman.d/flufflinux-update-state.json";
pub const LAST_UPDATE: &str = "/etc/pacman.d/lastupdate.json";
pub const LAST_UPDATE_KEY: &str = "last_successful_system_update";
pub const LOCK: &str = "/var/lib/pacman/db.lck";
pub const POLICY: &str = "/etc/pacman.d/flufflinux-update-package-protection.json";
pub const KEY_LOCK: &str = "/run/flufflinux-update-key-recovery.lock";
pub const MAX_OUTPUT: usize = 8 * 1024 * 1024;

pub fn read_json(path: impl AsRef<Path>) -> Value {
    fs::read(path)
        .ok()
        .and_then(|data| serde_json::from_slice(&data).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| serde_json::json!({}))
}

pub fn atomic_write(path: impl AsRef<Path>, data: &[u8]) -> io::Result<()> {
    let path = path.as_ref();
    let mut temporary = tempfile::NamedTempFile::new_in(path.parent().unwrap_or(Path::new(".")))?;
    let permissions = fs::metadata(path)
        .map(|m| m.permissions())
        .unwrap_or_else(|_| fs::Permissions::from_mode(0o644));
    temporary.as_file().set_permissions(permissions)?;
    temporary.write_all(data)?;
    temporary.as_file().sync_all()?;
    temporary.persist(path).map_err(|error| error.error)?;
    Ok(())
}

pub fn write_json(path: impl AsRef<Path>, value: &Value) -> bool {
    serde_json::to_vec_pretty(value)
        .ok()
        .is_some_and(|mut data| {
            data.push(b'\n');
            atomic_write(path, &data).is_ok()
        })
}

pub fn process_alive(pid: i64) -> bool {
    pid > 1 && pid <= i32::MAX as i64 && {
        // SAFETY: signal zero only probes an integer process ID.
        (unsafe { libc::kill(pid as libc::pid_t, 0) == 0 })
            || io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
    }
}

pub fn pacman_running() -> bool {
    fs::read_dir("/proc")
        .ok()
        .into_iter()
        .flatten()
        .flatten()
        .any(|entry| {
            entry.file_name().to_string_lossy().parse::<u32>().is_ok()
                && (fs::read_to_string(entry.path().join("comm"))
                    .is_ok_and(|s| s.trim() == "pacman")
                    || fs::read_link(entry.path().join("exe"))
                        .is_ok_and(|p| p == Path::new(PACMAN)))
        })
}

/// Both output channels share one OS pipe, preserving the Pacman prompt/log
/// stream. The bounded channel avoids unbounded buffering behind a slow reader.
pub struct Process {
    child: Child,
    input: Option<ChildStdin>,
    output: Receiver<Vec<u8>>,
    eof: bool,
    status: Option<ExitStatus>,
}

impl Process {
    pub fn spawn(
        program: &str,
        arguments: &[String],
        environment: &[(&str, &str)],
    ) -> io::Result<Self> {
        let mut descriptors = [0; 2];
        // SAFETY: pipe writes two valid descriptors into the supplied array.
        if unsafe { libc::pipe(descriptors.as_mut_ptr()) } != 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: each descriptor is uniquely transferred into an owned File.
        let mut reader = unsafe { File::from_raw_fd(descriptors[0]) };
        let writer = unsafe { File::from_raw_fd(descriptors[1]) };
        for descriptor in descriptors {
            // SAFETY: these newly created descriptors remain owned above.
            unsafe { libc::fcntl(descriptor, libc::F_SETFD, libc::FD_CLOEXEC) };
        }
        let mut child = Command::new(program)
            .args(arguments)
            .env("LC_ALL", "C")
            .envs(environment.iter().copied())
            .stdin(Stdio::piped())
            .stdout(Stdio::from(writer.try_clone()?))
            .stderr(Stdio::from(writer))
            .spawn()?;
        let input = child.stdin.take();
        if let Some(ref input) = input {
            // SAFETY: nonblocking writes to this owned pipe permit bounded
            // prompt answers even if a child stops reading its standard input.
            unsafe { libc::fcntl(input.as_raw_fd(), libc::F_SETFL, libc::O_NONBLOCK) };
        }
        let (sender, output) = mpsc::sync_channel(32);
        std::thread::spawn(move || {
            let mut data = [0; 8192];
            loop {
                match reader.read(&mut data) {
                    Ok(0) => break,
                    Ok(count) => {
                        if sender.send(data[..count].to_vec()).is_err() {
                            break;
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                    Err(_) => break,
                }
            }
        });
        Ok(Self {
            child,
            input,
            output,
            eof: false,
            status: None,
        })
    }

    pub fn receive(&mut self, wait: Duration) -> Vec<u8> {
        match self.output.recv_timeout(wait) {
            Ok(data) => data,
            Err(RecvTimeoutError::Disconnected) => {
                self.eof = true;
                Vec::new()
            }
            Err(RecvTimeoutError::Timeout) => Vec::new(),
        }
    }

    pub fn finished(&mut self) -> bool {
        if self.status.is_none() {
            self.status = self.child.try_wait().ok().flatten();
        }
        self.status.is_some() && self.eof
    }

    pub fn exit_code(&self) -> i32 {
        self.status.and_then(|s| s.code()).unwrap_or(-1)
    }

    pub fn answer(&mut self, answer: &[u8], close: bool, deadline: Instant) -> bool {
        let deadline = deadline.min(Instant::now() + Duration::from_secs(5));
        let Some(ref mut input) = self.input else {
            return false;
        };
        let mut remaining = answer;
        while !remaining.is_empty() && Instant::now() < deadline {
            match input.write(remaining) {
                Ok(0) => return false,
                Ok(count) => remaining = &remaining[count..],
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    std::thread::sleep(Duration::from_millis(2))
                }
                Err(_) => return false,
            }
        }
        if !remaining.is_empty() {
            return false;
        }
        if close {
            self.input.take();
        }
        true
    }

    pub fn stop(&mut self) {
        self.input.take();
        if self.status.is_none() {
            let _ = self.child.kill();
            self.status = self.child.wait().ok();
        }
    }
}

impl Drop for Process {
    fn drop(&mut self) {
        self.stop();
    }
}

pub fn arguments(values: &[&str]) -> Vec<String> {
    values.iter().map(|s| (*s).into()).collect()
}

pub fn run(
    program: &str,
    args: &[String],
    timeout: Duration,
    environment: &[(&str, &str)],
) -> CommandResult {
    let Ok(mut process) = Process::spawn(program, args, environment) else {
        return CommandResult {
            exit_code: -1,
            ..Default::default()
        };
    };
    let mut output = Vec::new();
    let deadline = Instant::now().checked_add(timeout);
    while !process.finished() {
        let data = process.receive(Duration::from_millis(10));
        if deadline.is_some_and(|deadline| Instant::now() >= deadline)
            || data.len() > MAX_OUTPUT.saturating_sub(output.len())
        {
            process.stop();
            return CommandResult {
                started: true,
                finished: false,
                exit_code: -1,
                output,
            };
        }
        output.extend(data);
    }
    CommandResult {
        started: true,
        finished: true,
        exit_code: process.exit_code(),
        output,
    }
}

/// Same-origin HTTPS, system CA roots, bounded response and total timeout.
pub fn fetch_artifact(
    url: &str,
    maximum: usize,
    oversized: &str,
    unavailable: &str,
) -> Result<Vec<u8>, String> {
    let expected = reqwest::Url::parse(url).map_err(|_| unavailable.to_string())?;
    if expected.scheme() != "https" {
        return Err(unavailable.into());
    }
    let origin = expected.clone();
    let policy = reqwest::redirect::Policy::custom(move |attempt| {
        if attempt.previous().len() >= 10
            || attempt.url().scheme() != "https"
            || attempt.url().host_str() != origin.host_str()
            || attempt.url().port_or_known_default() != origin.port_or_known_default()
        {
            attempt.error("cross-origin or excessive redirect")
        } else {
            attempt.follow()
        }
    });
    let client = reqwest::blocking::Client::builder()
        .redirect(policy)
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|_| unavailable.to_string())?;
    let response = client
        .get(expected)
        .send()
        .and_then(|r| r.error_for_status())
        .map_err(|_| unavailable.to_string())?;
    let mut data = Vec::new();
    response
        .take(maximum as u64 + 1)
        .read_to_end(&mut data)
        .map_err(|_| unavailable.to_string())?;
    if data.len() > maximum {
        Err(oversized.into())
    } else {
        Ok(data)
    }
}

pub struct NativeContext;
pub fn archive_name(location: &str) -> Option<String> {
    let url = reqwest::Url::parse(location).ok()?;
    let name = url.path_segments()?.next_back()?;
    Some(
        percent_encoding::percent_decode_str(name)
            .decode_utf8_lossy()
            .into_owned(),
    )
}
impl Context for NativeContext {
    fn fingerprint(&self) -> Result<Vec<u8>, String> {
        fetch_artifact(
            "https://fluffnet.org/flufflinux-fnrepo/packages/flufflinux-signing-key.fingerprint",
            4096,
            "fingerprint-response-oversized",
            "fingerprint-endpoint-unavailable",
        )
    }
    fn certificate(&self) -> Result<Vec<u8>, String> {
        fetch_artifact(
            "https://fluffnet.org/flufflinux-fnrepo/packages/flufflinux-signing-key.asc",
            256 * 1024,
            "certificate-response-oversized",
            "certificate-endpoint-unavailable",
        )
    }
    fn run(&self, program: &str, args: &[String]) -> CommandResult {
        run(program, args, Duration::from_secs(30), &[])
    }
}

/// A guard serializes stale-lock reclamation. The visible lock keeps Qt's
/// PID/application/hostname format, so an older running helper is respected.
pub struct RecoveryLock {
    path: PathBuf,
    file: File,
    _guard: File,
}
impl RecoveryLock {
    pub fn acquire(path: &Path) -> io::Result<Self> {
        let guard_path = path.with_extension("lock.guard");
        let guard = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(guard_path)?;
        let deadline = Instant::now() + Duration::from_millis(100);
        loop {
            if guard.try_lock_exclusive().is_ok() {
                break;
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "recovery lock busy",
                ));
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        if path.exists() {
            let owner = fs::read_to_string(path)
                .ok()
                .and_then(|s| s.lines().next()?.parse::<i64>().ok());
            if owner.is_none_or(|pid| pid <= 1 || process_alive(pid)) {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "recovery lock busy",
                ));
            }
            fs::remove_file(path)?;
        }
        let mut file = OpenOptions::new()
            .create_new(true)
            .read(true)
            .write(true)
            .mode(0o600)
            .open(path)?;
        let mut hostname = [0u8; 256];
        // SAFETY: gethostname receives a writable, bounded byte buffer.
        unsafe { libc::gethostname(hostname.as_mut_ptr().cast(), hostname.len()) };
        let hostname = String::from_utf8_lossy(&hostname)
            .trim_end_matches('\0')
            .to_string();
        write!(
            file,
            "{}\nflufflinux-update\n{hostname}\n",
            std::process::id()
        )?;
        Ok(Self {
            path: path.into(),
            file,
            _guard: guard,
        })
    }
}
impl Drop for RecoveryLock {
    fn drop(&mut self) {
        if let (Ok(open), Ok(named)) = (self.file.metadata(), fs::symlink_metadata(&self.path)) {
            if open.ino() == named.ino() && open.dev() == named.dev() {
                let _ = fs::remove_file(&self.path);
            }
        }
    }
}

pub fn recover_key(output: &str) -> RecoveryResult {
    let repository = signing_key::repository_name(output);
    if repository != "fluffnet" {
        return signing_key::recover(&NativeContext, &Config::default(), output);
    }
    let Ok(_lock) = RecoveryLock::acquire(Path::new(KEY_LOCK)) else {
        return RecoveryResult {
            repository,
            category: "recovery-already-running".into(),
            requested_fingerprint: signing_key::requested_fingerprint(output),
            ..Default::default()
        };
    };
    signing_key::recover(&NativeContext, &Config::default(), output)
}

pub fn current_update_date() -> String {
    // libc preserves the operating system's timezone abbreviation (e.g. IDT),
    // matching the existing lastupdate hook rather than inventing a new format.
    let timestamp = chrono::Local::now().timestamp() as libc::time_t;
    let mut local = std::mem::MaybeUninit::<libc::tm>::uninit();
    let mut buffer = [0u8; 128];
    // SAFETY: all pointers refer to live, appropriately sized storage and the
    // format is a static, NUL-terminated string.
    unsafe {
        if libc::localtime_r(&timestamp, local.as_mut_ptr()).is_null() {
            return String::new();
        }
        let length = libc::strftime(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            c"%Y-%m-%d %H:%M:%S %Z %z".as_ptr(),
            local.as_ptr(),
        );
        String::from_utf8_lossy(&buffer[..length]).into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn recovery_lock_excludes_and_releases() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("key.lock");
        let first = RecoveryLock::acquire(&path).unwrap();
        assert!(RecoveryLock::acquire(&path).is_err());
        drop(first);
        assert!(!path.exists());
        assert!(RecoveryLock::acquire(&path).is_ok());
    }
    #[test]
    fn live_and_malformed_legacy_locks_are_not_removed() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("key.lock");
        for content in [
            format!("{}\nlegacy\nhost\n", std::process::id()),
            "invalid\n".into(),
            "1\n".into(),
        ] {
            fs::write(&path, &content).unwrap();
            assert!(RecoveryLock::acquire(&path).is_err());
            assert_eq!(fs::read_to_string(&path).unwrap(), content);
        }
    }
    #[test]
    fn process_io_combines_channels_and_reports_status() {
        let result = run(
            "/bin/sh",
            &arguments(&["-c", "printf first; printf second >&2; exit 7"]),
            Duration::from_secs(5),
            &[],
        );
        assert!(result.started && result.finished);
        assert_eq!(result.exit_code, 7);
        assert_eq!(result.output, b"firstsecond");
    }
    #[test]
    fn process_timeout_is_not_reported_as_success() {
        let result = run(
            "/bin/sh",
            &arguments(&["-c", "sleep 1"]),
            Duration::from_millis(20),
            &[],
        );
        assert!(result.started);
        assert!(!result.finished);
        assert_ne!(result.exit_code, 0);
    }
    #[test]
    fn non_https_artifacts_are_rejected_before_fetching() {
        assert_eq!(
            fetch_artifact("http://127.0.0.1/key", 10, "large", "unavailable"),
            Err("unavailable".into())
        );
        assert_eq!(
            fetch_artifact("file:///etc/passwd", 10, "large", "unavailable"),
            Err("unavailable".into())
        );
    }
    #[test]
    fn atomic_state_preserves_permissions_and_unrelated_fields() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("state");
        fs::write(&path, b"old").unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o640)).unwrap();
        assert!(write_json(
            &path,
            &serde_json::json!({"phase":"downloading","other":"keep"})
        ));
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o640
        );
        assert_eq!(read_json(&path)["other"], "keep");
    }
}
