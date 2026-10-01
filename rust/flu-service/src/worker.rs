//! Persistent update worker. It survives closing System Settings and publishes
//! the same atomic state/log contract consumed by the QML application.
mod power;

use flu_core::{
    runtime::*,
    signing_key::{self, RecoveryResult},
};
use regex::Regex;
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    fs::{self, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
    time::{Duration, Instant},
};

const LOG: &str = "/etc/pacman.d/flufflinux-update.log";
const PACMAN_LOG: &str = "/var/log/pacman.log";
const CACHE: &str = "/var/cache/pacman/pkg";

fn append_log(text: &str) {
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(LOG) {
        let _ = file.write_all(text.as_bytes());
    }
}
fn human_speed(bytes: i64) -> String {
    if bytes >= 1024 * 1024 {
        format!("{:.1} MiB/s", bytes as f64 / (1024.0 * 1024.0))
    } else if bytes >= 1024 {
        format!("{:.1} KiB/s", bytes as f64 / 1024.0)
    } else {
        format!("{bytes} B/s")
    }
}
fn integer(state: &Value, key: &str) -> i64 {
    state[key].as_i64().unwrap_or(0)
}

struct Worker {
    state: Value,
    packages: BTreeMap<String, i64>,
    total_bytes: i64,
    recent_deltas: VecDeque<i64>,
    attempted_keys: BTreeSet<String>,
    log_offset: u64,
    log_buffer: String,
    log_completed: i64,
    output_completed: i64,
    file_recoveries: u32,
}
impl Worker {
    fn new() -> Self {
        let mut state = read_json(STATE);
        state["worker_pid"] = json!(std::process::id());
        let _ = fs::write(
            LOG,
            format!(
                "Fluff Linux Update diagnostic log\n{}\n\n",
                chrono::Local::now().format("%Y-%m-%dT%H:%M:%S%:z")
            ),
        );
        Self {
            state,
            packages: BTreeMap::new(),
            total_bytes: 0,
            recent_deltas: VecDeque::new(),
            attempted_keys: BTreeSet::new(),
            log_offset: 0,
            log_buffer: String::new(),
            log_completed: 0,
            output_completed: 0,
            file_recoveries: 0,
        }
    }
    fn publish(&self) {
        write_json(STATE, &self.state);
    }
    fn phase(&mut self, phase: &str) {
        self.state["phase"] = json!(phase);
        self.publish();
    }
    fn fail(&mut self, error: &str) {
        self.state["error"] = json!(error);
        self.state["speed"] = json!("");
        self.phase("failed");
    }
    fn fail_key(&mut self, result: &RecoveryResult, status: i32) {
        for (key, value, maximum) in [
            ("security_repository", &result.repository, 64),
            ("security_failure_category", &result.category, 128),
            (
                "security_expected_fingerprint",
                &result.expected_fingerprint,
                40,
            ),
            (
                "security_received_fingerprint",
                &result.received_fingerprint,
                40,
            ),
            (
                "security_requested_fingerprint",
                &result.requested_fingerprint,
                40,
            ),
        ] {
            self.state[key] = json!(value.chars().take(maximum).collect::<String>());
        }
        self.state["security_pacman_exit_status"] = json!(status);
        self.fail("SIGNING_KEY_VERIFICATION_FAILED");
    }
    /// Some(true): retry; Some(false): terminal security failure; None: not a key error.
    fn recover_signing_key(&mut self, output: &str, status: i32) -> Option<bool> {
        let unknown = signing_key::contains_unknown_key_report(output);
        let repository = signing_key::repository_name(output);
        let requested = signing_key::requested_fingerprint(output);
        let category = if unknown && repository != "fluffnet" {
            if repository.is_empty() {
                "repository-unidentified"
            } else {
                "repository-not-fluffnet"
            }
        } else if requested.is_empty() {
            if !unknown {
                return None;
            }
            "requested-fingerprint-invalid"
        } else if !self.attempted_keys.insert(requested.clone()) {
            "recovery-retry-failed"
        } else {
            ""
        };
        let result = if category.is_empty() {
            recover_key(output)
        } else {
            RecoveryResult {
                repository,
                requested_fingerprint: requested,
                category: category.into(),
                ..Default::default()
            }
        };
        if !result.recovered {
            self.fail_key(&result, status);
            return Some(false);
        }
        append_log(&format!(
            "\n[verified repository signing key imported] {}\n",
            result.received_fingerprint
        ));
        self.state["security_repository"] = json!(result.repository);
        for key in [
            "security_failure_category",
            "security_expected_fingerprint",
            "security_received_fingerprint",
            "security_requested_fingerprint",
            "security_pacman_exit_status",
        ] {
            self.state.as_object_mut().unwrap().remove(key);
        }
        self.state["recovery_notice_type"] = json!("signing-key-verified");
        self.state["recovery_notice_id"] = json!(chrono::Utc::now().timestamp_millis());
        self.publish();
        std::thread::sleep(Duration::from_millis(300));
        Some(true)
    }
    fn prepare(&mut self) -> bool {
        loop {
            self.phase("starting");
            let result = run(
                PACMAN,
                &arguments(&["-Syup", "--noconfirm", "--print-format", "%l|FLUFF|%s"]),
                Duration::from_secs(120),
                &[],
            );
            let output = String::from_utf8_lossy(&result.output);
            append_log(&format!("[transaction preparation]\n{output}\n"));
            if !result.started || !result.finished || result.exit_code != 0 {
                match self.recover_signing_key(&output, result.exit_code) {
                    Some(true) => continue,
                    Some(false) => return false,
                    None => {
                        self.fail("TRANSACTION_PREPARE_FAILED");
                        return false;
                    }
                }
            }
            for line in output.lines() {
                let Some((location, size)) = line.rsplit_once("|FLUFF|") else {
                    continue;
                };
                let Ok(url) = reqwest_url(location.trim()) else {
                    continue;
                };
                let name = url;
                if let Ok(size) = size.trim().parse::<i64>() {
                    if name.contains(".pkg.tar.") && !name.ends_with(".sig") && size >= 0 {
                        self.packages.insert(name, size);
                    }
                }
            }
            self.total_bytes = self.packages.values().sum();
            self.state["total_packages"] = json!(self.packages.len());
            self.state["total_download_bytes"] = json!(self.total_bytes);
            return true;
        }
    }
    fn cached_bytes(&self) -> i64 {
        self.packages
            .iter()
            .filter(|(_, size)| **size > 0)
            .map(|(name, expected)| {
                let complete = Path::new(CACHE).join(name);
                let partial = PathBuf::from(format!("{}.part", complete.display()));
                fs::metadata(complete)
                    .or_else(|_| fs::metadata(partial))
                    .map(|m| (m.len() as i64).min(*expected))
                    .unwrap_or(0)
            })
            .sum()
    }
    fn completed_downloads(&self) -> i64 {
        self.packages
            .keys()
            .filter(|name| Path::new(CACHE).join(name).exists())
            .count() as i64
    }
    fn download_progress(&self, bytes: i64) -> f64 {
        if self.total_bytes > 0 {
            (100.0 * bytes as f64 / self.total_bytes as f64).clamp(0.0, 100.0)
        } else if self.packages.is_empty() {
            100.0
        } else {
            100.0 * self.completed_downloads() as f64 / self.packages.len() as f64
        }
    }
    fn download_snapshot(&mut self, bytes: i64) {
        self.state["completed_packages"] = json!(self.completed_downloads());
        self.state["downloaded_bytes"] = json!(bytes);
        self.state["progress"] = json!(self.download_progress(bytes));
        self.publish();
    }
    fn download(&mut self) -> bool {
        loop {
            append_log("\n[download]\n");
            self.state["phase"] = json!("downloading");
            let mut last_bytes = self.cached_bytes();
            self.download_snapshot(last_bytes);
            let Ok(mut process) = Process::spawn(
                PACMAN,
                &arguments(&["-Syu", "--downloadonly", "--noconfirm", "--color", "never"]),
                &[],
            ) else {
                self.fail("DOWNLOAD_FAILED");
                return false;
            };
            let mut output = String::new();
            let mut tick = Instant::now();
            while !process.finished() {
                let chunk = process.receive(Duration::from_millis(20));
                let chunk = String::from_utf8_lossy(&chunk);
                append_log(&chunk);
                append_bounded(&mut output, &chunk);
                if tick.elapsed() >= Duration::from_secs(1) {
                    tick = Instant::now();
                    let bytes = self.cached_bytes();
                    self.recent_deltas.push_back((bytes - last_bytes).max(0));
                    last_bytes = bytes;
                    while self.recent_deltas.len() > 3 {
                        self.recent_deltas.pop_front();
                    }
                    let average = self.recent_deltas.iter().sum::<i64>()
                        / self.recent_deltas.len().max(1) as i64;
                    self.state["speed"] = json!(human_speed(average));
                    self.download_snapshot(bytes);
                }
            }
            if process.exit_code() == 0 {
                return true;
            }
            match self.recover_signing_key(&output, process.exit_code()) {
                Some(true) => continue,
                Some(false) => return false,
                None => {
                    let lower = output.to_lowercase();
                    let connection = [
                        "failed retrieving file",
                        "could not resolve host",
                        "failed to connect",
                        "connection timed out",
                        "network is unreachable",
                    ]
                    .iter()
                    .any(|s| lower.contains(s));
                    self.fail(if connection {
                        "DOWNLOAD_CONNECTION_FAILED"
                    } else {
                        "DOWNLOAD_FAILED"
                    });
                    return false;
                }
            }
        }
    }
    fn install_progress(&mut self, completed: i64, reported_total: i64) {
        if reported_total > 0 {
            self.state["total_packages"] = json!(reported_total);
        }
        let total = integer(&self.state, "total_packages");
        let visible = integer(&self.state, "completed_packages").max(completed);
        self.state["completed_packages"] = json!(if total > 0 {
            visible.min(total)
        } else {
            visible
        });
        if total > 0 {
            self.state["progress"] = json!(100.0 * visible.min(total) as f64 / total as f64);
        }
        self.publish();
    }
    fn read_install_log(&mut self) {
        let Ok(mut log) = fs::File::open(PACMAN_LOG) else {
            return;
        };
        if log
            .metadata()
            .map(|m| m.len() < self.log_offset)
            .unwrap_or(false)
        {
            self.log_offset = 0;
        }
        if log.seek(SeekFrom::Start(self.log_offset)).is_err() {
            return;
        }
        let mut data = Vec::new();
        if log.read_to_end(&mut data).is_err() {
            return;
        }
        self.log_offset += data.len() as u64;
        self.log_buffer.push_str(&String::from_utf8_lossy(&data));
        let operation =
            Regex::new(r"\[ALPM\]\s+(?:upgraded|installed|downgraded|reinstalled|removed)\s+")
                .unwrap();
        let mut completed = 0;
        while let Some(newline) = self.log_buffer.find('\n') {
            if operation.is_match(&self.log_buffer[..newline]) {
                completed += 1;
            }
            self.log_buffer.drain(..=newline);
        }
        if completed > 0 {
            self.log_completed += completed;
            self.install_progress(self.log_completed, 0);
        }
    }
    fn process_install_output(&mut self, pending: &mut String, chunk: &str) {
        pending.push_str(&chunk.replace('\r', "\n"));
        let operation = Regex::new(r"\(\s*(\d+)\s*/\s*(\d+)\s*\)\s+(?:upgrading|downgrading|installing|reinstalling|removing)").unwrap();
        while let Some(newline) = pending.find('\n') {
            if let Some(found) = operation.captures(&pending[..newline]) {
                let completed = found[1].parse::<i64>().unwrap_or(0);
                let total = found[2].parse::<i64>().unwrap_or(0);
                if total > 0 {
                    self.output_completed = self.output_completed.max(completed);
                    self.install_progress(self.output_completed, total);
                }
            }
            pending.drain(..=newline);
        }
    }
    fn file_conflict(&mut self, output: &str) -> bool {
        if self.file_recoveries >= 25 {
            append_log("\n[file recovery limit reached]\n");
            return false;
        }
        let expression =
            Regex::new(r"(?:^|\n)[^:\n]+:\s+(/[^\n]+?)\s+exists in filesystem").unwrap();
        let Some(found) = expression.captures(output) else {
            return false;
        };
        let original = clean_path(Path::new(found[1].trim()));
        if original == Path::new("/")
            || !original.is_absolute()
            || fs::symlink_metadata(&original).is_err()
        {
            return false;
        }
        if pacman_running() {
            return false;
        }
        // An owned-file conflict is not an obsolete unmanaged file. Only
        // Pacman's explicit "No package owns" result permits preservation.
        // Failed, timed-out or ambiguous ownership checks fail closed.
        let owner = run(
            PACMAN,
            &arguments(&["-Qo", "--", &original.to_string_lossy()]),
            Duration::from_secs(10),
            &[],
        );
        if !owner.started
            || !owner.finished
            || owner.exit_code != 1
            || !String::from_utf8_lossy(&owner.output)
                .to_lowercase()
                .contains("no package owns")
        {
            return false;
        }
        for _ in 0..100 {
            // Retain the existing filename contract without overwriting files.
            let mut random = [0_u8; 4];
            if fs::File::open("/dev/urandom")
                .and_then(|mut file| file.read_exact(&mut random))
                .is_err()
            {
                return false;
            }
            let suffix = 10000 + u32::from_ne_bytes(random) % 90000;
            let preserved = PathBuf::from(format!("{}.preupdate{suffix}", original.display()));
            if rename_without_replace(&original, &preserved).is_err() {
                continue;
            }
            self.file_recoveries += 1;
            append_log(&format!(
                "\n[file conflict recovered]\n{} -> {}\n",
                original.display(),
                preserved.display()
            ));
            self.state["recovery_original_file"] = json!(original.to_string_lossy());
            self.state["recovery_preserved_file"] = json!(preserved.to_string_lossy());
            self.state["recovery_notice_type"] = json!("file-conflict");
            self.state["recovery_notice_id"] = json!(chrono::Utc::now().timestamp_millis());
            self.state["phase"] = json!("installing");
            self.state["progress"] = json!(0);
            self.state["completed_packages"] = json!(0);
            // The failed child was reaped before this retry; do not clear a
            // lock belonging to a separately started, live Pacman process.
            if pacman_running() {
                return false;
            }
            let _ = fs::remove_file(LOCK);
            self.publish();
            std::thread::sleep(Duration::from_millis(300));
            return true;
        }
        append_log(&format!(
            "\n[file recovery failed] {}\n",
            original.display()
        ));
        false
    }
    fn install(&mut self) {
        loop {
            append_log("\n[installation]\n");
            self.state["phase"] = json!("installing");
            self.state["completed_packages"] = json!(0);
            self.state["progress"] = json!(0);
            self.state["speed"] = json!("");
            self.publish();
            self.log_offset = fs::metadata(PACMAN_LOG).map(|m| m.len()).unwrap_or(0);
            let Ok(mut process) = Process::spawn(
                PACMAN,
                &arguments(&["-Syu", "--noconfirm", "--color", "never"]),
                &[],
            ) else {
                self.fail("INSTALL_FAILED");
                return;
            };
            let mut output = String::new();
            let mut pending = String::new();
            let mut tick = Instant::now();
            while !process.finished() {
                let data = process.receive(Duration::from_millis(10));
                let chunk = String::from_utf8_lossy(&data);
                append_log(&chunk);
                append_bounded(&mut output, &chunk);
                self.process_install_output(&mut pending, &chunk);
                if tick.elapsed() >= Duration::from_millis(250) {
                    tick = Instant::now();
                    self.read_install_log();
                }
            }
            self.process_install_output(&mut pending, "\n");
            self.read_install_log();
            if process.exit_code() == 0 {
                self.state["progress"] = json!(100);
                self.state["completed_packages"] = self.state["total_packages"].clone();
                self.state["error"] = json!("");
                self.phase("complete");
                return;
            }
            match self.recover_signing_key(&output, process.exit_code()) {
                Some(true) => continue,
                Some(false) => return,
                None => {
                    if self.file_conflict(&output) {
                        continue;
                    }
                }
            }
            self.fail("INSTALL_FAILED");
            return;
        }
    }
}
fn append_bounded(output: &mut String, chunk: &str) {
    output.push_str(chunk);
    if output.len() > MAX_OUTPUT {
        let mut offset = output.len() - MAX_OUTPUT;
        while !output.is_char_boundary(offset) {
            offset += 1;
        }
        output.drain(..offset);
    }
}
fn clean_path(path: &Path) -> PathBuf {
    let mut result = PathBuf::new();
    for part in path.components() {
        match part {
            Component::ParentDir => {
                result.pop();
            }
            Component::CurDir => {}
            _ => result.push(part),
        }
    }
    result
}
fn reqwest_url(location: &str) -> Result<String, ()> {
    // Pacman prints a URL for each archive. Names are safe single path
    // components, including percent-encoded Unicode names.
    let url = flu_core::runtime::archive_name(location).ok_or(())?;
    if url.contains('/') || url == ".." {
        Err(())
    } else {
        Ok(url)
    }
}
fn rename_without_replace(original: &Path, preserved: &Path) -> std::io::Result<()> {
    use std::os::unix::ffi::OsStrExt;
    let source = std::ffi::CString::new(original.as_os_str().as_bytes())?;
    let destination = std::ffi::CString::new(preserved.as_os_str().as_bytes())?;
    // SAFETY: both paths are valid NUL-terminated strings. The exclusive
    // rename flag prevents overwriting another file in a racing filesystem.
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
        Err(std::io::Error::last_os_error())
    }
}
fn main() {
    let mut worker = Worker::new();
    // Acquire before any Pacman operation, and keep the descriptor alive across
    // preparation, download, installation, package hooks and recovery retries.
    // Closing the application does not affect this system-service-owned lock.
    let inhibitor = match power::SleepInhibitor::acquire() {
        Ok(inhibitor) => inhibitor,
        Err(error) => {
            append_log(&format!("[sleep inhibition failed] {error}\n"));
            worker.fail("SLEEP_INHIBITOR_FAILED");
            return;
        }
    };
    append_log("[sleep inhibition acquired]\n");
    if worker.prepare() && worker.download() {
        worker.install();
    }
    drop(inhibitor);
    append_log("[sleep inhibition released]\n");
}
