//! Polkit entry point. All privileged decisions and operations are Rust.
use base64::{
    Engine,
    engine::general_purpose::{URL_SAFE, URL_SAFE_NO_PAD},
};
use flu_core::{
    runtime::*,
    signing_key::{self, RecoveryResult},
};
use regex::Regex;
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    fs,
    io::{self, Write},
    path::Path,
    process::ExitCode,
    time::{Duration, Instant},
};

const DATABASE_PREFIX: &str = "/tmp/flufflinux-checkupdates-";

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Removal {
    Protected,
    Warning,
    Automatic,
}

struct Policy {
    protected: BTreeSet<String>,
    warning: BTreeSet<String>,
    valid: bool,
}
impl Policy {
    fn read() -> Self {
        let document = fs::read(POLICY)
            .ok()
            .and_then(|data| serde_json::from_slice::<Value>(&data).ok());
        let names = |key: &str| {
            document
                .as_ref()
                .and_then(|d| d[key].as_array())
                .into_iter()
                .flatten()
                .filter_map(Value::as_str)
                .filter(|s| !s.is_empty())
                .map(str::to_string)
                .collect()
        };
        Self {
            protected: names("protected"),
            warning: names("warning"),
            valid: document.is_some_and(|d| d.is_object()),
        }
    }
    fn classify(&self, package: &str) -> Removal {
        if !self.valid || self.protected.contains(package) {
            Removal::Protected
        } else if self.warning.contains(package) {
            Removal::Warning
        } else {
            Removal::Automatic
        }
    }
}

fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"@._+:-".contains(&c))
}
fn decode(value: &str) -> Vec<u8> {
    URL_SAFE
        .decode(value)
        .or_else(|_| URL_SAFE_NO_PAD.decode(value))
        .unwrap_or_default()
}
fn print_output(bytes: &[u8]) {
    let mut out = io::stdout().lock();
    let _ = out.write_all(bytes);
    let _ = out.flush();
}
fn command(args: &[&str], seconds: u64) -> flu_core::signing_key::CommandResult {
    run(PACMAN, &arguments(args), Duration::from_secs(seconds), &[])
}
fn successful(result: &flu_core::signing_key::CommandResult) -> bool {
    result.started && result.finished && result.exit_code == 0
}
fn failure_marker(result: &RecoveryResult, status: i32) {
    println!(
        "FLU_SIGNING_KEY_FAILURE:{}|{}|{}|{}|{}|{status}",
        result.repository,
        result.category,
        result.expected_fingerprint,
        result.received_fingerprint,
        result.requested_fingerprint
    );
}
fn recover_for_check(encoded: &str) -> i32 {
    let decoded = decode(encoded);
    if decoded.is_empty() || decoded.len() > 1024 * 1024 {
        println!("FLU_SIGNING_KEY_FAILURE:|check-output-invalid||||1");
        return 24;
    }
    let result = recover_key(&String::from_utf8_lossy(&decoded));
    if result.recovered {
        0
    } else {
        failure_marker(&result, 1);
        24
    }
}

fn remove_package(package: &str) -> i32 {
    if !valid_name(package) {
        eprintln!("INVALID_PACKAGE");
        return 2;
    }
    if pacman_running() {
        eprintln!("PACMAN_RUNNING");
        return 3;
    }
    let _ = fs::remove_file(LOCK);
    let result = command(&["-Rdd", "--noconfirm", package], u64::MAX / 4);
    if !result.started || !result.finished {
        eprintln!("PACKAGE_REMOVAL_FAILED");
        return 1;
    }
    print_output(&result.output);
    if result.exit_code < 0 {
        1
    } else {
        result.exit_code
    }
}

fn installed_version(package: &str) -> String {
    let result = command(&["-Q", package], 5);
    if !successful(&result) {
        return String::new();
    }
    String::from_utf8_lossy(&result.output)
        .split_whitespace()
        .last()
        .unwrap_or("")
        .into()
}
fn sync_version(database: &str, package: &str) -> String {
    let result = command(&["--dbpath", database, "-Si", package], 5);
    if !successful(&result) {
        return String::new();
    }
    let regex = Regex::new(r"(?:^|\n)Version\s*:\s*(\S+)").unwrap();
    regex
        .captures(&String::from_utf8_lossy(&result.output))
        .map(|m| m[1].into())
        .unwrap_or_default()
}
fn planned_packages(database: &str) -> Vec<(String, String)> {
    let result = command(
        &[
            "--dbpath",
            database,
            "-Sup",
            "--noconfirm",
            "--print-format",
            "%n|%v",
        ],
        15,
    );
    if !successful(&result) {
        return Vec::new();
    }
    String::from_utf8_lossy(&result.output)
        .lines()
        .filter_map(|line| {
            let (name, version) = line.trim().split_once('|')?;
            (valid_name(name) && !version.is_empty() && !version.chars().any(char::is_whitespace))
                .then(|| (name.into(), version.into()))
        })
        .collect()
}

fn stop_planner(process: &mut Process, lock: &Path) {
    process.stop();
    let _ = fs::remove_file(lock);
}
fn abort_planner(process: &mut Process, lock: &Path, output: &mut Vec<u8>, reason: &str) -> i32 {
    stop_planner(process, lock);
    let until = Instant::now() + Duration::from_millis(100);
    while !process.finished() && Instant::now() < until {
        let chunk = process.receive(Duration::from_millis(5));
        output
            .extend_from_slice(&chunk[..chunk.len().min(MAX_OUTPUT.saturating_sub(output.len()))]);
    }
    print_output(output);
    println!("\nFLU_TRANSACTION_SUMMARY_ABORT:{reason}");
    25
}
fn removal_result(policy: &Policy, package: &str, dependency: bool, output: &[u8]) -> i32 {
    match policy.classify(package) {
        Removal::Protected => {
            print_output(output);
            println!("\nFLU_PROTECTED_REMOVAL:{package}");
            20
        }
        Removal::Warning => {
            print_output(output);
            println!(
                "\nFLU_WARNING_{}:{package}",
                if dependency { "DEPENDENCY" } else { "REMOVAL" }
            );
            21
        }
        Removal::Automatic => {
            if remove_package(package) != 0 {
                print_output(output);
                println!("\nFLU_AUTOREMOVE_FAILED:{package}");
                22
            } else {
                println!("FLU_AUTOREMOVED:{package}");
                23
            }
        }
    }
}

fn transaction_summary(database: &str, signing_retry: bool) -> i32 {
    let uid = std::env::var("PKEXEC_UID").unwrap_or_default();
    if uid.parse::<u32>().is_err() {
        eprintln!("flufflinux-update-helper: missing invoking user");
        return 2;
    }
    if database != format!("{DATABASE_PREFIX}{uid}") {
        eprintln!("flufflinux-update-helper: invalid database path");
        return 2;
    }
    if pacman_running() {
        eprintln!("PACMAN_RUNNING");
        return 3;
    }
    let temporary_lock = Path::new(database).join("db.lck");
    let _ = fs::remove_file(&temporary_lock);
    let deadline = Instant::now() + Duration::from_secs(120);
    let Ok(mut process) = Process::spawn(
        PACMAN,
        &arguments(&["--dbpath", database, "-Su", "--color", "never"]),
        &[],
    ) else {
        eprintln!("flufflinux-update-helper: could not start pacman");
        let _ = fs::remove_file(temporary_lock);
        return 1;
    };
    let policy = Policy::read();
    let replacement = Regex::new(r"(?i)Replace\s+([A-Za-z0-9@._+:-]+)\s+with\s+(?:[A-Za-z0-9@._+:-]+/)?([A-Za-z0-9@._+:-]+)\?\s*\[Y/n\]").unwrap();
    let removal = Regex::new(r"(?i)Remove\s+([A-Za-z0-9@._+:-]+)\?\s*\[y/N\]").unwrap();
    let mut output = Vec::new();
    let mut parsed = 0;
    let mut final_answered = false;
    let mut replacements = Vec::new();
    while !process.finished() {
        if Instant::now() >= deadline {
            return abort_planner(&mut process, &temporary_lock, &mut output, "timeout");
        }
        let data = process.receive(Duration::from_millis(100));
        if data.len() > MAX_OUTPUT.saturating_sub(output.len()) {
            return abort_planner(&mut process, &temporary_lock, &mut output, "output-limit");
        }
        output.extend(data);
        let text = String::from_utf8_lossy(&output);
        let unparsed = &text[parsed.min(text.len())..];
        let prompt_end = signing_key::signing_key_import_prompt_end(unparsed);
        if prompt_end >= 0 {
            if !process.answer(b"n\n", true, deadline) {
                return abort_planner(
                    &mut process,
                    &temporary_lock,
                    &mut output,
                    "prompt-write-failed",
                );
            }
            // The shared parser returns QString-compatible UTF-16 positions.
            let mut units = 0;
            let byte_end = unparsed
                .char_indices()
                .find_map(|(index, character)| {
                    units += character.len_utf16() as i64;
                    (units == prompt_end).then_some(index + character.len_utf8())
                })
                .unwrap_or(unparsed.len());
            parsed += byte_end;
            continue;
        }
        if let Some(found) = replacement.captures(unparsed) {
            replacements.push((found[1].to_string(), found[2].to_string()));
            if !process.answer(b"y\n", false, deadline) {
                return abort_planner(
                    &mut process,
                    &temporary_lock,
                    &mut output,
                    "prompt-write-failed",
                );
            }
            parsed += found.get(0).unwrap().end();
            continue;
        }
        if let Some(found) = removal.captures(unparsed) {
            let package = found[1].to_string();
            stop_planner(&mut process, &temporary_lock);
            return removal_result(&policy, &package, false, &output);
        }
        if !final_answered && unparsed.contains("Proceed with installation?") {
            if !process.answer(b"n\n", true, deadline) {
                return abort_planner(
                    &mut process,
                    &temporary_lock,
                    &mut output,
                    "prompt-write-failed",
                );
            }
            final_answered = true;
        }
    }
    let status = process.exit_code();
    let text = String::from_utf8_lossy(&output);
    if status != 0 && signing_key::contains_unknown_key_report(&text) {
        let repository = signing_key::repository_name(&text);
        let result = if signing_retry && repository == "fluffnet" {
            RecoveryResult {
                repository,
                category: "recovery-retry-failed".into(),
                requested_fingerprint: signing_key::requested_fingerprint(&text),
                ..Default::default()
            }
        } else {
            recover_key(&text)
        };
        if result.recovered {
            let _ = fs::remove_file(temporary_lock);
            return transaction_summary(database, true);
        }
        failure_marker(&result, status);
        return 24;
    }
    for (old, new) in replacements {
        let old_version = installed_version(&old);
        let new_version = sync_version(database, &new);
        if !old_version.is_empty() && !new_version.is_empty() {
            println!("FLU_REPLACEMENT:{old}|{old_version}|{new}|{new_version}");
        }
    }
    for (package, version) in planned_packages(database) {
        println!("FLU_PLANNED_PACKAGE:{package}|{version}");
    }
    if status != 0 {
        let required = Regex::new(r"required by\s+([A-Za-z0-9@._+:-]+)").unwrap();
        let blockers: BTreeSet<_> = required
            .captures_iter(&text)
            .map(|m| m[1].to_string())
            .collect();
        // Check protected/warning blockers before performing any automatic
        // removal. Hash iteration must never decide which safety class wins.
        if let Some(package) = blockers.iter().min_by_key(|p| policy.classify(p)) {
            return removal_result(&policy, package, true, &output);
        }
    }
    print_output(&output);
    if status < 0 { 1 } else { status }
}

fn start_installation(download: &str, storage: &str, freed: bool, updates: &str) -> i32 {
    let previous = read_json(STATE);
    let phase = previous["phase"].as_str().unwrap_or("");
    if (matches!(phase, "starting" | "downloading" | "installing")
        && process_alive(previous["worker_pid"].as_i64().unwrap_or(0)))
        || pacman_running()
    {
        eprintln!("PACMAN_RUNNING");
        return 3;
    }
    let _ = fs::remove_file(LOCK);
    let packages: Value = serde_json::from_slice(&decode(updates))
        .ok()
        .filter(Value::is_array)
        .unwrap_or(json!([]));
    let mut state = json!({"phase":"starting", "progress":0, "completed_packages":0, "total_packages":0,
        "speed":"", "download_size":download.chars().take(128).collect::<String>(),
        "storage_change":storage.chars().take(128).collect::<String>(), "storage_freed":freed, "updates":packages, "error":""});
    if !write_json(STATE, &state) {
        eprintln!("STATE_WRITE_FAILED");
        return 1;
    }
    let result = run(
        "/usr/bin/systemctl",
        &arguments(&["start", "flufflinux-update.service"]),
        Duration::from_secs(30),
        &[],
    );
    if !successful(&result) {
        state["phase"] = json!("failed");
        state["error"] = json!("WORKER_START_FAILED");
        write_json(STATE, &state);
        eprintln!("WORKER_START_FAILED");
        return 1;
    }
    0
}
fn cancel_installation() -> i32 {
    let mut state = read_json(STATE);
    if state["phase"] != "downloading" {
        eprintln!("CANCEL_NOT_ALLOWED");
        return 4;
    }
    let result = run(
        "/usr/bin/systemctl",
        &arguments(&["stop", "flufflinux-update.service"]),
        Duration::from_secs(5),
        &[],
    );
    if !successful(&result) {
        eprintln!("CANCEL_FAILED");
        return 1;
    }
    if pacman_running() {
        eprintln!("PACMAN_RUNNING");
        return 3;
    }
    if Path::new(LOCK).exists() && fs::remove_file(LOCK).is_err() {
        eprintln!("CANCEL_FAILED");
        return 1;
    }
    state["phase"] = json!("cancelled");
    state["progress"] = json!(0);
    state["speed"] = json!("");
    state["error"] = json!("");
    write_json(STATE, &state);
    0
}
fn record_update() -> i32 {
    let mut state = read_json(LAST_UPDATE);
    state[LAST_UPDATE_KEY] = json!(current_update_date());
    if write_json(LAST_UPDATE, &state) {
        0
    } else {
        eprintln!("LAST_UPDATE_WRITE_FAILED");
        1
    }
}
fn execute(args: &[String]) -> i32 {
    match args {
        [operation, output] if operation == "--recover-signing-key" => recover_for_check(output),
        [database] | [database, _] if database.starts_with(DATABASE_PREFIX) => {
            transaction_summary(database, false)
        }
        [operation, download, storage, freed, updates] if operation == "--install" => {
            start_installation(download, storage, freed == "true", updates)
        }
        [operation] if operation == "--cancel" => cancel_installation(),
        [operation, package] if operation == "--remove-package" => remove_package(package),
        [operation] if operation == "--record-current-update" => record_update(),
        _ => {
            eprintln!("flufflinux-update-helper: invalid arguments");
            2
        }
    }
}
fn main() -> ExitCode {
    ExitCode::from(execute(&std::env::args().skip(1).collect::<Vec<_>>()) as u8)
}
