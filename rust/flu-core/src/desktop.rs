//! Desktop controller: all update state, orchestration and presentation data.
//! Qt/KDE only supplies translation and publishes snapshots on its GUI thread.
use crate::{battery::BatteryMonitor, runtime::*, signing_key};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use chrono::{DateTime, Local, Months};
use notify::{RecursiveMode, Watcher};
use regex::Regex;
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    fs,
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, Sender},
    },
    time::{Duration, Instant},
};
pub type Translator = Arc<dyn Fn(&str, &str, i64, &[String]) -> String + Send + Sync>;
pub const UNKNOWN: &str =
    "An unknown error has occurred. Please report this issue on our GitHub page for assistance.";
pub const SECURITY: &str = "Fluff Linux Update could not verify the FluffNet repository signing key. The update was stopped to protect your system.";
const VERIFIED: &str =
    "The official FluffNet repository signing key was verified and added to Pacman.";

#[derive(Clone, Debug)]
pub enum Command {
    Check,
    Clear,
    Install,
    Cancel,
    RetryKey,
    Resolve(bool),
    Refresh,
    Network(bool, bool),
}

pub fn initial_model() -> Value {
    json!({"lastUpdate":"","hasLastUpdate":false,"stateMessage":"","freshnessText":"","freshnessColor":"#3daee9","relativeTime":"",
    "checking":false,"checkComplete":false,"updatesAvailable":false,"downloadSize":"","diskChange":"","diskSpaceFreed":false,"checkError":"",
    "updatePackages":[],"batteryLow":false,"installPhase":"idle","updateActive":false,"installProgress":0.0,"completedPackages":0,"totalPackages":0,
    "downloadedSize":"0 B","totalDownloadSize":"0 B","downloadSpeed":"","installError":"","signingKeySecurityError":false,"signingKeyTechnicalDetails":"",
    "signingKeyIssueUrl":"","cancellationNotice":false,"installationSuccessNotice":false,"networkConnected":true,"networkLimited":false,
    "recoveryDialogType":"","recoveryPackage":"","recoveryNotice":"","recoveryActionState":""})
}
pub fn human_size(bytes: i64) -> String {
    for (unit, divisor) in [("GiB", 1073741824), ("MiB", 1048576), ("KiB", 1024)] {
        if bytes >= divisor {
            let v = format!("{:.1}", bytes as f64 / divisor as f64);
            return format!("{} {unit}", v.trim_end_matches(".0"));
        }
    }
    format!("{} B", bytes.max(0))
}
pub fn friendly_error(output: &str) -> &'static str {
    let output = output.to_lowercase();
    if [
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
    ]
    .iter()
    .any(|s| output.contains(s))
    {
        "No internet connection. Check your network and try again."
    } else if [
        "failed retrieving file",
        "failed to synchronize all databases",
        "failed to update database",
        "failed to download",
        "could not find database",
        "the requested url returned error",
        "too many errors from",
    ]
    .iter()
    .any(|s| output.contains(s))
    {
        "The repository servers could not be reached. Try again later or check your mirror configuration."
    } else {
        UNKNOWN
    }
}
pub fn authorized_error(code: i32, output: &str) -> bool {
    let output = output.to_lowercase();
    matches!(code, 126 | 127)
        || [
            "not authorized",
            "not allowed",
            "authentication failed",
            "dismissed",
        ]
        .iter()
        .any(|s| output.contains(s))
}
pub fn parse_packages(output: &str) -> Vec<Value> {
    let expression = Regex::new(r"(?m)^\s*(\S+)\s+(\S+)\s+->\s+(\S+)\s*$").unwrap();
    expression
        .captures_iter(output)
        .map(|m| json!({"name":&m[1],"currentVersion":&m[2],"newName":&m[1],"newVersion":&m[3]}))
        .collect()
}
pub fn summary_packages(packages: &mut Vec<Value>, output: &str) {
    let replacement = Regex::new(
        r"FLU_REPLACEMENT:([A-Za-z0-9@._+:-]+)\|([^|\s]+)\|([A-Za-z0-9@._+:-]+)\|([^|\s]+)",
    )
    .unwrap();
    let mut new_names = std::collections::BTreeSet::new();
    for m in replacement.captures_iter(output) {
        new_names.insert(m[3].to_owned());
        packages.retain(|p| p["name"] != m[1] && p["name"] != m[3]);
        packages
            .push(json!({"name":&m[1],"currentVersion":&m[2],"newName":&m[3],"newVersion":&m[4]}));
    }
    let planned = Regex::new(r"FLU_PLANNED_PACKAGE:([A-Za-z0-9@._+:-]+)\|([^|\s]+)").unwrap();
    for m in planned.captures_iter(output) {
        if !new_names.contains(&m[1])
            && !packages
                .iter()
                .any(|p| p["name"] == m[1] || p["newName"] == m[1])
        {
            packages
                .push(json!({"name":&m[1],"currentVersion":"","newName":&m[1],"newVersion":&m[2]}));
        }
    }
}
pub fn parse_ini(path: &Path) -> BTreeMap<String, BTreeMap<String, String>> {
    let mut result = BTreeMap::new();
    let mut section = String::new();
    for line in fs::read_to_string(path).unwrap_or_default().lines() {
        let line = line.trim();
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].into();
        } else if !line.starts_with(['#', ';']) {
            if let Some((key, value)) = line.split_once('=') {
                result
                    .entry(section.clone())
                    .or_insert_with(BTreeMap::new)
                    .insert(key.trim().into(), value.trim().into());
            }
        }
    }
    result
}
pub fn save_settings(path: &Path, section: &str, values: &[(&str, String)]) -> bool {
    let Some(parent) = path.parent() else {
        return false;
    };
    if fs::create_dir_all(parent).is_err() {
        return false;
    }
    let mut ini = parse_ini(path);
    for (key, value) in values {
        ini.entry(section.into())
            .or_default()
            .insert((*key).into(), value.clone());
    }
    let mut output = String::new();
    for (section, values) in ini {
        if !section.is_empty() {
            output.push_str(&format!("[{section}]\n"));
        }
        for (key, value) in values {
            output.push_str(&format!("{key}={value}\n"));
        }
        output.push('\n');
    }
    atomic_write(path, output.as_bytes()).is_ok()
}
pub fn sanitize(value: &str, maximum: usize) -> String {
    let clean: String = value
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let clean = Regex::new(r"(?i)(?:/home/|/Users/)[^\s]+")
        .unwrap()
        .replace_all(&clean, "[redacted-path]");
    let clean = Regex::new(r"(?i)(?:password|passwd|token|secret|authorization)\s*[:=]\s*[^\s]+")
        .unwrap()
        .replace_all(&clean, "[redacted-secret]");
    clean
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(maximum)
        .collect()
}
fn os_version() -> String {
    fs::read_to_string("/etc/os-release")
        .unwrap_or_default()
        .lines()
        .find_map(|s| s.strip_prefix("PRETTY_NAME="))
        .unwrap_or("unknown")
        .trim()
        .trim_matches('"')
        .into()
}

/// The only URL the signing-key interface can open. Diagnostic fields are
/// explicitly whitelisted, bounded and sanitized before URL encoding.
pub fn issue_url(fields: &[String], status: i32) -> String {
    let labels = [
        "Repository",
        "FLU version",
        "OS version",
        "Expected fingerprint",
        "Received fingerprint",
        "Requested signing-subkey fingerprint",
        "Failure category",
    ];
    let limits = [64, 32, 160, 40, 40, 40, 128];
    let mut lines: Vec<_> = labels
        .iter()
        .zip(limits)
        .enumerate()
        .map(|(index, (label, limit))| {
            format!(
                "{label}: {}",
                sanitize(fields.get(index).map(String::as_str).unwrap_or(""), limit)
            )
        })
        .collect();
    lines.push(format!("Pacman exit status: {status}"));
    let mut url =
        reqwest::Url::parse("https://github.com/FluffNet/flufflinux-update/issues/new").unwrap();
    url.query_pairs_mut()
        .append_pair("title", "Repository signing-key verification failed")
        .append_pair("body", &lines.join("\n"));
    // Match QUrl's encoding, including consumers that do not treat '+' as a
    // space in a query component. Literal plus signs are already %2B.
    url.to_string().replace('+', "%20")
}

pub fn issue_url_allowed(value: &str) -> bool {
    reqwest::Url::parse(value).is_ok_and(|url| {
        url.scheme() == "https"
            && url.host_str() == Some("github.com")
            && url.path() == "/FluffNet/flufflinux-update/issues/new"
    })
}
pub struct Controller {
    pub model: Value,
    translate: Translator,
    publish: Box<dyn Fn(Value) + Send>,
    stop: Arc<AtomicBool>,
    ignore_terminal: bool,
    notice_until: Option<Instant>,
    cancel_until: Option<Instant>,
    success_until: Option<Instant>,
    last_notice: i64,
    recovery_attempted: bool,
    pending_removed: Vec<String>,
    check_started: Instant,
    battery: BatteryMonitor,
}
impl Controller {
    pub fn new(
        translate: Translator,
        publish: impl Fn(Value) + Send + 'static,
        stop: Arc<AtomicBool>,
    ) -> Self {
        Self {
            model: initial_model(),
            translate,
            publish: Box::new(publish),
            stop,
            ignore_terminal: true,
            notice_until: None,
            cancel_until: None,
            success_until: None,
            last_notice: 0,
            recovery_attempted: false,
            pending_removed: vec![],
            check_started: Instant::now(),
            battery: BatteryMonitor::default(),
        }
    }
    fn tr(&self, message: &str) -> String {
        (self.translate)(message, "", -1, &[])
    }
    fn arg(&self, message: &str, args: &[String]) -> String {
        (self.translate)(message, "", -1, args)
    }
    fn emit(&mut self) {
        self.model["updateActive"] = json!(self.active());
        // Recompute eligibility before every published transition, not just
        // on the periodic tick: checking, completion and failure must never
        // publish a stale low-battery warning from the previous phase.
        self.model["batteryLow"] = json!(self.battery.is_low(self.battery_warning_relevant()));
        (self.publish)(self.model.clone());
    }
    fn battery_warning_relevant(&self) -> bool {
        self.active()
            || (matches!(self.string("installPhase"), "idle" | "cancelled")
                && self.boolean("checkComplete")
                && !self.boolean("checking")
                && self.string("checkError").is_empty()
                && self.boolean("updatesAvailable"))
    }
    fn active(&self) -> bool {
        matches!(
            self.string("installPhase"),
            "starting" | "downloading" | "installing"
        )
    }
    fn string(&self, key: &str) -> &str {
        self.model[key].as_str().unwrap_or("")
    }
    fn boolean(&self, key: &str) -> bool {
        self.model[key].as_bool().unwrap_or(false)
    }
    fn notice(&mut self, text: String) {
        self.model["recoveryNotice"] = json!(text);
        self.notice_until = Some(Instant::now() + Duration::from_secs(10));
    }
    fn execute(
        &self,
        program: &str,
        args: &[String],
        environment: &[(&str, &str)],
    ) -> signing_key::CommandResult {
        let Ok(mut process) = Process::spawn(program, args, environment) else {
            return signing_key::CommandResult::default();
        };
        let mut result = signing_key::CommandResult {
            started: true,
            ..Default::default()
        };
        let started = Instant::now();
        loop {
            result
                .output
                .extend(process.receive(Duration::from_millis(100)));
            if process.finished() {
                result.finished = true;
                result.exit_code = process.exit_code();
                break;
            }
            if result.output.len() > MAX_OUTPUT
                || started.elapsed() > Duration::from_secs(300)
                || self.stop.load(Ordering::Relaxed)
            {
                process.stop();
                break;
            }
        }
        result
    }
    fn helper(&self, args: &[String]) -> signing_key::CommandResult {
        let mut arguments = vec![HELPER.to_owned()];
        arguments.extend_from_slice(args);
        self.execute("pkexec", &arguments, &[])
    }
    fn finish_check(&mut self) {
        while self.check_started.elapsed() < Duration::from_millis(850)
            && !self.stop.load(Ordering::Relaxed)
        {
            std::thread::sleep(Duration::from_millis(20));
        }
        self.model["checking"] = json!(false);
        self.model["checkComplete"] = json!(true);
        if self.string("checkError").is_empty()
            && self.boolean("updatesAvailable")
            && !self.pending_removed.is_empty()
        {
            self.pending_removed.sort();
            self.pending_removed.dedup();
            let message = if self.pending_removed.len() == 1 {
                "To allow system updates to continue, %1 was automatically removed after it was deemed safe to remove."
            } else {
                "To allow system updates to continue, the following packages were automatically removed after they were deemed safe to remove: %1"
            };
            self.notice(self.arg(message, &[self.pending_removed.join(", ")]));
            self.pending_removed.clear();
        }
        self.emit();
    }
    fn fail_check(&mut self, message: &str) {
        self.model["checkError"] = json!(self.tr(message));
        self.finish_check();
    }
    // Mirrors the six-field helper wire protocol plus presentation context.
    #[allow(clippy::too_many_arguments)]
    fn security(
        &mut self,
        repository: &str,
        category: &str,
        expected: &str,
        received: &str,
        requested: &str,
        code: i32,
        checking: bool,
    ) {
        self.model["signingKeySecurityError"] = json!(true);
        let fields = [
            ("Repository: %1", repository, 64),
            ("Failure category: %1", category, 128),
            ("Expected fingerprint: %1", expected, 40),
            ("Received fingerprint: %1", received, 40),
            ("Requested signing-key fingerprint: %1", requested, 40),
        ];
        let mut details = Vec::new();
        for (message, value, _) in fields {
            if !value.is_empty() || message == "Failure category: %1" {
                details.push(self.arg(message, &[format!("\u{2066}{value}\u{2069}")]))
            }
        }
        for (message, value) in [
            ("Pacman exit status: %1", code.to_string()),
            ("FLU version: %1", env!("CARGO_PKG_VERSION").into()),
            ("Fluff Linux version: %1", os_version()),
        ] {
            details.push(self.arg(message, &[format!("\u{2066}{value}\u{2069}")]))
        }
        self.model["signingKeyTechnicalDetails"] = json!(details.join("\n"));
        self.model["signingKeyIssueUrl"] = json!(issue_url(
            &[
                repository.into(),
                env!("CARGO_PKG_VERSION").into(),
                os_version(),
                expected.into(),
                received.into(),
                requested.into(),
                category.into()
            ],
            code
        ));
        if checking {
            self.model["updatesAvailable"] = json!(false);
            self.fail_check(SECURITY)
        } else {
            self.model["installError"] = json!(self.tr(SECURITY));
        }
    }
    fn security_token(&mut self, output: &str, checking: bool) -> bool {
        let token=Regex::new(r"FLU_SIGNING_KEY_FAILURE:([^|\n]*)\|([^|\n]*)\|([^|\n]*)\|([^|\n]*)\|([^|\n]*)\|(-?\d+)").unwrap();
        if let Some(m) = token.captures(output) {
            self.security(
                &m[1],
                &m[2],
                &m[3],
                &m[4],
                &m[5],
                m[6].parse().unwrap_or(-1),
                checking,
            );
            true
        } else {
            false
        }
    }
    fn recover_check(&mut self, output: &str, code: i32) -> bool {
        let repository = signing_key::repository_name(output);
        let requested = signing_key::requested_fingerprint(output);
        if self.recovery_attempted {
            self.security(
                &repository,
                "recovery-retry-failed",
                "",
                "",
                &requested,
                code,
                true,
            );
            return false;
        }
        self.recovery_attempted = true;
        let result = self.helper(&[
            "--recover-signing-key".into(),
            URL_SAFE_NO_PAD.encode(output),
        ]);
        if result.started && result.finished && result.exit_code == 0 {
            self.notice(self.tr(VERIFIED));
            true
        } else {
            if !self.security_token(&String::from_utf8_lossy(&result.output), true) {
                self.security(
                    &repository,
                    "recovery-helper-failed",
                    "",
                    "",
                    &requested,
                    code,
                    true,
                );
            }
            false
        }
    }
    fn check(&mut self, restart: bool) {
        if self.boolean("checking") || !self.boolean("networkConnected") || self.active() {
            return;
        }
        if !restart {
            self.recovery_attempted = false;
            self.pending_removed.clear();
            self.model["recoveryActionState"] = json!("");
        }
        self.success_until = None;
        self.cancel_until = None;
        self.ignore_terminal = true;
        for field in [
            "installationSuccessNotice",
            "cancellationNotice",
            "signingKeySecurityError",
            "updatesAvailable",
            "checkComplete",
            "diskSpaceFreed",
        ] {
            self.model[field] = json!(false);
        }
        for field in [
            "installError",
            "signingKeyTechnicalDetails",
            "signingKeyIssueUrl",
            "downloadSize",
            "diskChange",
            "checkError",
        ] {
            self.model[field] = json!("");
        }
        self.model["installPhase"] = json!("idle");
        self.model["updatePackages"] = json!([]);
        if pacman_running() {
            self.fail_check("pacman process is already running.");
            return;
        }
        self.model["checking"] = json!(true);
        self.check_started = Instant::now();
        self.emit();
        // The DB path and installed local link are intentionally unchanged.
        let db = format!("/tmp/flufflinux-checkupdates-{}", unsafe {
            libc::geteuid()
        });
        if fs::create_dir_all(&db).is_err() {
            self.fail_check(
                "The update checker could not be started. Make sure pacman-contrib is installed.",
            );
            return;
        }
        let local = Path::new(&db).join("local");
        if !local.exists() {
            let _ = std::os::unix::fs::symlink("/var/lib/pacman/local", local);
        }
        let environment = [("CHECKUPDATES_DB", db.as_str())];
        let ansi = Regex::new(r"\x1B\[[0-?]*[ -/]*[@-~]").unwrap();
        let autoremove = Regex::new(r"FLU_AUTOREMOVED:([A-Za-z0-9@._+:-]+)").unwrap();
        let size = Regex::new(r"Total Download Size:\s*([0-9]+(?:\.[0-9]+)?)\s*([^\s]+)").unwrap();
        let net = Regex::new(r"Net Upgrade Size:\s*(-?[0-9]+(?:\.[0-9]+)?)\s*([^\s]+)").unwrap();
        let removal_tokens: Vec<_> = [
            ("FLU_PROTECTED_REMOVAL:", "protected"),
            ("FLU_WARNING_REMOVAL:", "warning"),
            ("FLU_WARNING_DEPENDENCY:", "warning"),
        ]
        .into_iter()
        .map(|(marker, kind)| {
            (
                Regex::new(&format!(r"{marker}([A-Za-z0-9@._+:-]+)")).unwrap(),
                kind,
            )
        })
        .collect();
        loop {
            if self.stop.load(Ordering::Relaxed) {
                return;
            }
            let sync = self.execute(
                "fakeroot",
                &[
                    "--",
                    "pacman",
                    "-Sy",
                    "--noconfirm",
                    "--disable-sandbox-filesystem",
                    "--dbpath",
                    &db,
                    "--logfile",
                    "/dev/null",
                ]
                .map(String::from),
                &environment,
            );
            let output = String::from_utf8_lossy(&sync.output);
            if !sync.started {
                self.fail_check("The update checker could not be started. Make sure pacman-contrib is installed.");
                return;
            }
            if !sync.finished || sync.exit_code < 0 {
                self.fail_check("The update check stopped unexpectedly.");
                return;
            }
            if sync.exit_code != 0 {
                if signing_key::contains_unknown_key_report(&output)
                    && self.recover_check(&output, sync.exit_code)
                {
                    continue;
                }
                if !self.boolean("signingKeySecurityError") {
                    self.fail_check(friendly_error(&output));
                }
                return;
            }
            let query = self.execute(
                "checkupdates",
                &["--nosync".into(), "--nocolor".into()],
                &environment,
            );
            let output = String::from_utf8_lossy(&query.output);
            if !query.started {
                self.fail_check("The update checker could not be started. Make sure pacman-contrib is installed.");
                return;
            }
            if !query.finished || query.exit_code < 0 {
                self.fail_check("The update check stopped unexpectedly.");
                return;
            }
            if query.exit_code != 0 && signing_key::contains_unknown_key_report(&output) {
                if self.recover_check(&output, query.exit_code) {
                    continue;
                }
                return;
            }
            if query.exit_code == 2 {
                if !self.boolean("hasLastUpdate") {
                    let record = self.helper(&["--record-current-update".into()]);
                    if !record.started || !record.finished || record.exit_code != 0 {
                        self.fail_check(
                            if authorized_error(
                                record.exit_code,
                                &String::from_utf8_lossy(&record.output),
                            ) {
                                "Authorization error, please try again."
                            } else {
                                "The privileged update check could not be started."
                            },
                        );
                        return;
                    }
                    self.read_last();
                }
                self.finish_check();
                return;
            }
            if query.exit_code != 0 {
                self.fail_check(friendly_error(&output));
                return;
            }
            self.model["updatesAvailable"] = json!(true);
            self.model["updatePackages"] = json!(parse_packages(&output));
            self.emit();
            let summary = self.helper(&[db.clone(), String::new()]);
            let output = ansi
                .replace_all(&String::from_utf8_lossy(&summary.output), "")
                .into_owned();
            let mut packages = self.model["updatePackages"]
                .as_array()
                .cloned()
                .unwrap_or_default();
            summary_packages(&mut packages, &output);
            self.model["updatePackages"] = json!(packages);
            for (token, kind) in &removal_tokens {
                if let Some(m) = token.captures(&output) {
                    self.model["checking"] = json!(false);
                    self.model["checkComplete"] = json!(false);
                    self.model["recoveryDialogType"] = json!(kind);
                    self.model["recoveryActionState"] = json!(kind);
                    self.model["recoveryPackage"] = json!(&m[1]);
                    self.emit();
                    return;
                }
            }
            if let Some(m) = autoremove.captures(&output) {
                self.pending_removed.push(m[1].into());
                continue;
            }
            if self.security_token(&output, true) {
                return;
            }
            if !summary.started {
                self.fail_check("The privileged update check could not be started.");
                return;
            }
            if !summary.finished || summary.exit_code < 0 {
                self.fail_check("The privileged update check stopped unexpectedly.");
                return;
            }
            if authorized_error(summary.exit_code, &output) {
                self.fail_check("Authorization error, please try again.");
                return;
            }
            // Pacman returns 1 when the planner deliberately declines the
            // final install prompt. A complete size summary is still required
            // below; other helper failures must not enable installation.
            let preview_declined =
                summary.exit_code == 1 && output.contains("Proceed with installation?");
            if summary.exit_code != 0 && !preview_declined {
                self.fail_check(friendly_error(&output));
                return;
            }
            if let Some(m) = size.captures(&output) {
                self.model["downloadSize"] = json!(format!("{} {}", &m[1], &m[2]));
            } else if output.contains("Total Installed Size:") {
                self.model["downloadSize"] = json!(self.tr("No additional download required"));
            } else {
                self.fail_check(UNKNOWN);
                return;
            }
            if let Some(m) = net.captures(&output) {
                let value = m[1].parse::<f64>().unwrap_or(0.);
                self.model["diskSpaceFreed"] = json!(value < 0.);
                self.model["diskChange"] = json!(format!("{:.2} {}", value.abs(), &m[2]));
            } else {
                self.fail_check(UNKNOWN);
                return;
            }
            self.finish_check();
            return;
        }
    }
    pub fn command(&mut self, command: Command) {
        match command {
            Command::Network(connected, limited) => {
                self.model["networkConnected"] = json!(connected);
                self.model["networkLimited"] = json!(limited);
            }
            Command::Refresh => self.read_last(),
            Command::Check => self.check(false),
            Command::Clear
                if !self.boolean("checking") && !self.active() && self.boolean("checkComplete") =>
            {
                for field in ["checkComplete", "updatesAvailable", "diskSpaceFreed"] {
                    self.model[field] = json!(false);
                }
                for field in [
                    "downloadSize",
                    "diskChange",
                    "checkError",
                    "recoveryActionState",
                ] {
                    self.model[field] = json!("");
                }
                self.model["updatePackages"] = json!([]);
            }
            Command::Install
                if !self.active()
                    && self.boolean("updatesAvailable")
                    && self.string("checkError").is_empty()
                    && self.boolean("networkConnected") =>
            {
                self.success_until = None;
                self.cancel_until = None;
                self.ignore_terminal = true;
                self.model["installPhase"] = json!("starting");
                self.model["installationSuccessNotice"] = json!(false);
                self.model["cancellationNotice"] = json!(false);
                self.model["installError"] = json!("");
                self.model["signingKeySecurityError"] = json!(false);
                self.model["installProgress"] = json!(0);
                self.emit();
                let result = self.helper(&[
                    "--install".into(),
                    self.string("downloadSize").into(),
                    self.string("diskChange").into(),
                    self.boolean("diskSpaceFreed").to_string(),
                    URL_SAFE_NO_PAD
                        .encode(serde_json::to_vec(&self.model["updatePackages"]).unwrap()),
                ]);
                if !result.started || !result.finished || result.exit_code != 0 {
                    let output = String::from_utf8_lossy(&result.output);
                    self.model["installPhase"] = json!("failed");
                    self.model["installError"] =
                        json!(self.tr(if output.contains("PACMAN_RUNNING") {
                            "pacman process is already running."
                        } else if authorized_error(result.exit_code, &output) {
                            "Authorization error, please try again."
                        } else {
                            "The update process could not be started."
                        }));
                } else {
                    self.read_install();
                }
            }
            Command::Cancel if self.string("installPhase") == "downloading" => {
                let result = self.helper(&["--cancel".into()]);
                if !result.started || !result.finished || result.exit_code != 0 {
                    self.model["installError"] = json!(self.tr(
                        if authorized_error(
                            result.exit_code,
                            &String::from_utf8_lossy(&result.output)
                        ) {
                            "Authorization error, please try again."
                        } else {
                            "The update process could not be cancelled."
                        }
                    ));
                } else {
                    self.read_install();
                }
            }
            Command::RetryKey if self.boolean("signingKeySecurityError") => {
                if self.string("installPhase") == "idle" {
                    self.model["signingKeySecurityError"] = json!(false);
                    self.check(false);
                } else {
                    self.command(Command::Install);
                }
            }
            Command::Resolve(allow) => {
                let kind = self.string("recoveryDialogType").to_owned();
                let package = self.string("recoveryPackage").to_owned();
                self.model["recoveryDialogType"] = json!("");
                self.model["recoveryPackage"] = json!("");
                if kind == "protected" {
                    self.model["recoveryActionState"] = json!("protected");
                } else if kind == "warning" {
                    if !allow {
                        self.model["recoveryActionState"] = json!("warning");
                        self.model["checkComplete"] = json!(false);
                        self.model["updatesAvailable"] = json!(false);
                        self.model["checkError"] = json!("");
                    } else {
                        self.model["recoveryActionState"] = json!("");
                        self.model["checking"] = json!(true);
                        self.emit();
                        let result = self.helper(&["--remove-package".into(), package]);
                        self.model["checking"] = json!(false);
                        if result.started && result.finished && result.exit_code == 0 {
                            self.check(true);
                        } else {
                            self.model["updatesAvailable"] = json!(false);
                            self.fail_check(if String::from_utf8_lossy(&result.output).contains("PACMAN_RUNNING") {"pacman process is already running."}else{"The conflicting package could not be removed. Please seek support."});
                        }
                    }
                }
            }
            _ => {}
        }
        self.emit();
    }
    pub fn read_last(&mut self) {
        let mut last = String::new();
        let mut message = String::new();
        match fs::read(LAST_UPDATE) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                message = self.tr("System was not previously updated.")
            }
            Err(_) => message = self.tr("The last update information could not be read."),
            Ok(data) => match serde_json::from_slice::<Value>(&data) {
                Ok(v) if v.is_object() => {
                    last = v[LAST_UPDATE_KEY].as_str().unwrap_or("").trim().into();
                    if last.is_empty() {
                        message = self.tr("System was not previously updated.");
                    }
                }
                _ => message = self.tr("The last update information is invalid."),
            },
        }
        let mut color = "#3daee9";
        let mut relative = String::new();
        let freshness = if last.is_empty() {
            self.tr("Updates were not installed on this system")
        } else {
            let parsed = if last.len() >= 24 {
                last.get(..19)
                    .zip(last.get(last.len() - 5..))
                    .and_then(|(date, offset)| {
                        DateTime::parse_from_str(
                            &format!("{date} {offset}"),
                            "%Y-%m-%d %H:%M:%S %z",
                        )
                        .ok()
                    })
            } else {
                None
            };
            if let Some(date) = parsed {
                let now = Local::now();
                let seconds = (now.timestamp() - date.timestamp()).max(0);
                let (singular, plural, count) = if seconds < 60 {
                    ("%1 second ago", "%1 seconds ago", seconds)
                } else if seconds < 3600 {
                    ("%1 minute ago", "%1 minutes ago", seconds / 60)
                } else if seconds < 86400 {
                    ("%1 hour ago", "%1 hours ago", seconds / 3600)
                } else {
                    ("%1 day ago", "%1 days ago", seconds / 86400)
                };
                relative = (self.translate)(singular, plural, count, &[]);
                if now
                    .checked_sub_months(Months::new(3))
                    .is_some_and(|limit| date < limit)
                {
                    color = "#f2994a";
                    self.tr("Last update was installed more than three months ago")
                } else if now
                    .checked_sub_months(Months::new(1))
                    .is_some_and(|limit| date < limit)
                {
                    color = "#f2c94c";
                    self.tr("Last update was installed more than a month ago")
                } else {
                    color = "#27ae60";
                    self.tr("Updated recently")
                }
            } else {
                self.tr("Update date available")
            }
        };
        self.model["hasLastUpdate"] = json!(!last.is_empty());
        self.model["lastUpdate"] = json!(last);
        self.model["stateMessage"] = json!(message);
        self.model["freshnessText"] = json!(freshness);
        self.model["freshnessColor"] = json!(color);
        self.model["relativeTime"] = json!(relative);
    }
    pub fn read_install(&mut self) {
        let state = read_json(STATE);
        self.apply_install(&state);
    }
    pub fn apply_install(&mut self, state: &Value) {
        let phase = state["phase"].as_str().unwrap_or("");
        let active = matches!(phase, "starting" | "downloading" | "installing");
        if phase.is_empty() || self.ignore_terminal && !active {
            return;
        }
        if active {
            self.ignore_terminal = false;
        }
        let previous = self.string("installPhase").to_owned();
        self.model["installPhase"] = json!(phase);
        for (field, key) in [
            ("installProgress", "progress"),
            ("completedPackages", "completed_packages"),
            ("totalPackages", "total_packages"),
            ("downloadSpeed", "speed"),
        ] {
            self.model[field] = state[key].clone();
        }
        self.model["downloadedSize"] =
            json!(human_size(state["downloaded_bytes"].as_i64().unwrap_or(0)));
        self.model["totalDownloadSize"] = json!(human_size(
            state["total_download_bytes"].as_i64().unwrap_or(0)
        ));
        let id = state["recovery_notice_id"].as_i64().unwrap_or(0);
        if id > 0 && id != self.last_notice {
            self.last_notice = id;
            let original = state["recovery_original_file"].as_str().unwrap_or("");
            let preserved = state["recovery_preserved_file"].as_str().unwrap_or("");
            let text = if state["recovery_notice_type"] == "signing-key-verified" {
                self.tr(VERIFIED)
            } else if !original.is_empty() && !preserved.is_empty() {
                self.arg("A file conflict was detected and resolved. %1 was renamed to %2. The update process has restarted.",&[original.into(),preserved.into()])
            } else {
                state["recovery_notice"].as_str().unwrap_or("").into()
            };
            self.notice(text);
        }
        if state["updates"].as_array().is_some_and(|v| !v.is_empty()) {
            self.model["updatePackages"] = state["updates"].clone();
        }
        for (field, key) in [
            ("downloadSize", "download_size"),
            ("diskChange", "storage_change"),
        ] {
            if state[key].as_str().is_some_and(|s| !s.is_empty()) {
                self.model[field] = state[key].clone();
            }
        }
        if state["storage_change"]
            .as_str()
            .is_some_and(|s| !s.is_empty())
        {
            self.model["diskSpaceFreed"] = json!(state["storage_freed"].as_bool().unwrap_or(false));
        }
        self.model["installError"] = json!("");
        self.model["signingKeySecurityError"] = json!(false);
        if phase == "failed" {
            let error = state["error"].as_str().unwrap_or("");
            if error == "SIGNING_KEY_VERIFICATION_FAILED" {
                self.security(
                    state["security_repository"].as_str().unwrap_or(""),
                    state["security_failure_category"].as_str().unwrap_or(""),
                    state["security_expected_fingerprint"]
                        .as_str()
                        .unwrap_or(""),
                    state["security_received_fingerprint"]
                        .as_str()
                        .unwrap_or(""),
                    state["security_requested_fingerprint"]
                        .as_str()
                        .unwrap_or(""),
                    state["security_pacman_exit_status"].as_i64().unwrap_or(-1) as i32,
                    false,
                );
            } else {
                self.model["installError"]=json!(self.tr(match error {"DOWNLOAD_FAILED"|"DOWNLOAD_CONNECTION_FAILED"|"TRANSACTION_PREPARE_FAILED"=>"Connection failed while downloading updates. Check your network and try again.","INSTALL_FAILED"=>"The system update failed.",_=>"The update process could not be started."}));
            }
        } else if phase == "cancelled" {
            self.model["checkComplete"] = json!(true);
            self.model["updatesAvailable"] = json!(true);
            if previous != phase {
                self.model["cancellationNotice"] = json!(true);
                self.cancel_until = Some(Instant::now() + Duration::from_secs(5));
            }
        } else if phase == "complete" {
            self.model["checkComplete"] = json!(true);
            self.model["updatesAvailable"] = json!(false);
            if matches!(previous.as_str(), "starting" | "downloading" | "installing") {
                self.model["installationSuccessNotice"] = json!(true);
                self.success_until = Some(Instant::now() + Duration::from_secs(7));
            }
            self.read_last();
        } else if active {
            self.model["checkComplete"] = json!(true);
            self.model["updatesAvailable"] = json!(true);
        }
    }
    fn tick(&mut self) {
        for (deadline, key, empty) in [
            (&mut self.notice_until, "recoveryNotice", json!("")),
            (&mut self.cancel_until, "cancellationNotice", json!(false)),
            (
                &mut self.success_until,
                "installationSuccessNotice",
                json!(false),
            ),
        ] {
            if deadline.is_some_and(|t| Instant::now() >= t) {
                *deadline = None;
                self.model[key] = empty;
            }
        }
    }
}
fn state_file_changed(event: &notify::Event) -> bool {
    // Reading state produces Access events on Linux. Refreshing on those would
    // feed our own reads back into the watcher indefinitely.
    event.kind.is_create() || event.kind.is_modify() || event.kind.is_remove()
}

pub fn start_controller(
    translate: Translator,
    publish: impl Fn(Value) + Send + 'static,
    stop: Arc<AtomicBool>,
) -> Sender<Command> {
    let (sender, receiver): (Sender<Command>, Receiver<Command>) = mpsc::channel();
    let event_sender = sender.clone();
    std::thread::spawn(move || {
        let mut controller = Controller::new(translate, publish, stop.clone());
        controller.read_last();
        controller.read_install();
        controller.emit();
        let mut watcher =
            notify::recommended_watcher(move |event: notify::Result<notify::Event>| {
                if event.is_ok_and(|event| state_file_changed(&event)) {
                    let _ = event_sender.send(Command::Refresh);
                }
            })
            .ok();
        if let Some(ref mut watcher) = watcher {
            let _ = watcher.watch(Path::new("/etc/pacman.d"), RecursiveMode::NonRecursive);
        }
        while !stop.load(Ordering::Relaxed) {
            match receiver.recv_timeout(Duration::from_millis(750)) {
                Ok(Command::Refresh) => {
                    controller.read_last();
                    controller.read_install();
                }
                Ok(command) => controller.command(command),
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
                _ => {}
            }
            if controller.active() {
                controller.read_install();
            }
            controller.tick();
            controller.emit();
        }
    });
    sender
}

#[cfg(test)]
mod tests {
    use super::*;
    fn controller() -> Controller {
        Controller::new(
            Arc::new(|s, p, n, a| {
                let mut s = if n >= 0 && n != 1 {
                    p.to_owned()
                } else {
                    s.to_owned()
                };
                if n >= 0 {
                    s = s.replace("%1", &n.to_string());
                }
                for (i, v) in a.iter().enumerate() {
                    s = s.replace(&format!("%{}", i + 1), v);
                }
                s
            }),
            |_| {},
            Arc::new(AtomicBool::new(false)),
        )
    }
    #[test]
    fn sizes_match_existing_contract() {
        assert_eq!(human_size(-1), "0 B");
        assert_eq!(human_size(1048576), "1 MiB");
        assert_eq!(human_size(1536), "1.5 KiB");
    }
    #[test]
    fn reading_state_does_not_trigger_a_refresh_loop() {
        use notify::event::{AccessKind, AccessMode, CreateKind, ModifyKind, RemoveKind};
        use notify::{Event, EventKind};
        for kind in [
            AccessKind::Read,
            AccessKind::Open(AccessMode::Read),
            AccessKind::Close(AccessMode::Read),
        ] {
            assert!(!state_file_changed(&Event::new(EventKind::Access(kind))));
        }
        for kind in [
            EventKind::Create(CreateKind::File),
            EventKind::Modify(ModifyKind::Any),
            EventKind::Remove(RemoveKind::File),
        ] {
            assert!(state_file_changed(&Event::new(kind)));
        }
    }
    #[test]
    fn network_errors_have_priority() {
        assert_eq!(
            friendly_error("failed retrieving file: could not resolve host"),
            "No internet connection. Check your network and try again."
        );
        assert_eq!(
            friendly_error("failed to synchronize all databases"),
            "The repository servers could not be reached. Try again later or check your mirror configuration."
        );
    }
    #[test]
    fn replacements_and_dependencies_are_listed_once() {
        let mut p = parse_packages("old 1 -> 2\nsame 1 -> 2\n");
        summary_packages(
            &mut p,
            "FLU_REPLACEMENT:old|1|new|3\nFLU_PLANNED_PACKAGE:new|3\nFLU_PLANNED_PACKAGE:dependency|1\nFLU_PLANNED_PACKAGE:same|2\n",
        );
        assert_eq!(p.len(), 3);
        assert_eq!(p[1]["newName"], "new");
        assert_eq!(p[2]["currentVersion"], "");
    }
    #[test]
    fn settings_preserve_unknown_fields() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("settings.conf");
        fs::write(&path, "[Other]\nKeep=yes\n").unwrap();
        assert!(save_settings(
            &path,
            "Interface",
            &[("PacmanView", "true".into())]
        ));
        assert_eq!(parse_ini(&path)["Other"]["Keep"], "yes");
    }
    #[test]
    fn diagnostic_secrets_and_paths_are_redacted() {
        assert_eq!(
            sanitize("/home/mai/private token=abcdef\nOK", 160),
            "[redacted-path] [redacted-secret] OK"
        );
    }
    #[test]
    fn previous_terminal_state_is_not_restored() {
        let mut c = controller();
        c.apply_install(&json!({"phase":"complete"}));
        assert_eq!(c.string("installPhase"), "idle");
        assert!(!c.boolean("installationSuccessNotice"));
    }
    #[test]
    fn live_worker_reconnect_and_completion() {
        let mut c = controller();
        c.apply_install(&json!({"phase":"downloading","updates":[{"name":"p"}],"downloaded_bytes":1024,"total_download_bytes":2048}));
        assert!(c.active());
        assert!(c.boolean("updatesAvailable"));
        c.apply_install(&json!({"phase":"complete"}));
        assert!(!c.active());
        assert!(!c.boolean("updatesAvailable"));
        assert!(c.boolean("installationSuccessNotice"));
    }
    #[test]
    fn cancellation_retains_retry_and_notice() {
        let mut c = controller();
        c.apply_install(&json!({"phase":"starting"}));
        c.apply_install(&json!({"phase":"cancelled"}));
        assert!(c.boolean("updatesAvailable"));
        assert!(c.boolean("cancellationNotice"));
    }
    #[test]
    fn battery_warning_is_only_eligible_when_ready_or_updating() {
        let mut c = controller();
        assert!(!c.battery_warning_relevant());
        c.model["checkComplete"] = json!(true);
        assert!(!c.battery_warning_relevant());
        c.model["updatesAvailable"] = json!(true);
        assert!(c.battery_warning_relevant());
        c.model["checking"] = json!(true);
        assert!(!c.battery_warning_relevant());
        c.model["checking"] = json!(false);
        c.model["checkError"] = json!("check failed");
        assert!(!c.battery_warning_relevant());
        c.model["checkError"] = json!("");

        for phase in ["starting", "downloading", "installing"] {
            c.apply_install(&json!({"phase":phase}));
            assert!(c.battery_warning_relevant(), "{phase}");
        }
        // Cancelling a download returns to the ready-to-install/retry UI.
        c.apply_install(&json!({"phase":"cancelled"}));
        assert!(c.battery_warning_relevant());
        c.command(Command::Clear);
        assert!(!c.battery_warning_relevant());
        assert!(!c.boolean("batteryLow"));
    }
    #[test]
    fn battery_warning_clears_in_the_first_complete_or_failed_snapshot() {
        use std::sync::Mutex;
        for (phase, error) in [
            ("complete", ""),
            ("failed", "DOWNLOAD_FAILED"),
            ("failed", "INSTALL_FAILED"),
            ("failed", "SLEEP_INHIBITOR_FAILED"),
            ("failed", "SIGNING_KEY_VERIFICATION_FAILED"),
        ] {
            let published = Arc::new(Mutex::new(initial_model()));
            let snapshot = published.clone();
            let mut c = Controller::new(
                Arc::new(|message, _, _, _| message.to_owned()),
                move |model| *snapshot.lock().unwrap() = model,
                Arc::new(AtomicBool::new(false)),
            );
            c.apply_install(&json!({"phase":"downloading"}));
            c.model["batteryLow"] = json!(true);
            c.apply_install(&json!({"phase":phase,"error":error}));
            c.emit();
            assert!(!c.battery_warning_relevant(), "{phase}: {error}");
            assert_eq!(published.lock().unwrap()["batteryLow"], false);
            // A new attempt may warn again, without waiting for a new check.
            c.apply_install(&json!({"phase":"starting"}));
            assert!(c.battery_warning_relevant());
        }
    }
    #[test]
    fn battery_warning_clears_before_check_and_startup_failure_are_published() {
        for (phase, complete, checking, available, error) in [
            ("idle", false, false, false, ""),
            ("idle", false, true, false, ""),
            ("idle", true, false, true, "check failed"),
            ("idle", true, false, false, ""),
            // Helper/polkit failure sets phase directly, without worker state.
            ("failed", true, false, true, ""),
        ] {
            let mut c = controller();
            c.model["installPhase"] = json!(phase);
            c.model["checkComplete"] = json!(complete);
            c.model["checking"] = json!(checking);
            c.model["updatesAvailable"] = json!(available);
            c.model["checkError"] = json!(error);
            c.model["batteryLow"] = json!(true);
            c.emit();
            assert!(!c.boolean("batteryLow"));
        }
    }
    #[test]
    fn sleep_inhibitor_failure_reports_startup_error_and_allows_retry() {
        let mut c = controller();
        c.apply_install(&json!({"phase":"starting"}));
        c.apply_install(&json!({"phase":"failed","error":"SLEEP_INHIBITOR_FAILED"}));
        assert!(!c.active());
        assert!(c.boolean("updatesAvailable"));
        assert!(!c.boolean("installationSuccessNotice"));
        // Reuse the existing translated startup error; the diagnostic log
        // records the exact logind failure without exposing raw D-Bus errors.
        assert_eq!(
            c.string("installError"),
            "The update process could not be started."
        );
    }
    #[test]
    fn security_error_preserves_isolated_technical_values() {
        let mut c = controller();
        c.apply_install(&json!({"phase":"starting"}));
        c.apply_install(&json!({"phase":"failed","error":"SIGNING_KEY_VERIFICATION_FAILED","security_repository":"fluffnet","security_failure_category":"primary-fingerprint-mismatch","security_expected_fingerprint":"ABC","security_pacman_exit_status":1}));
        assert!(c.boolean("signingKeySecurityError"));
        assert!(
            c.string("signingKeyTechnicalDetails")
                .contains("\u{2066}ABC\u{2069}")
        );
        let url = reqwest::Url::parse(c.string("signingKeyIssueUrl")).unwrap();
        assert_eq!(url.host_str(), Some("github.com"));
        assert_eq!(url.path(), "/FluffNet/flufflinux-update/issues/new");
    }
    #[test]
    fn offline_check_does_not_launch_processes() {
        let mut c = controller();
        c.command(Command::Network(false, false));
        c.command(Command::Check);
        assert!(!c.boolean("checking"));
        assert!(!c.boolean("checkComplete"));
    }
    #[test]
    fn protected_dialog_cannot_authorize_removal() {
        let mut c = controller();
        c.model["recoveryDialogType"] = json!("protected");
        c.model["recoveryPackage"] = json!("system");
        c.command(Command::Resolve(true));
        assert_eq!(c.string("recoveryActionState"), "protected");
        assert_eq!(c.string("recoveryPackage"), "");
    }
    #[test]
    fn rejection_leaves_action_required() {
        let mut c = controller();
        c.model["recoveryDialogType"] = json!("warning");
        c.command(Command::Resolve(false));
        assert_eq!(c.string("recoveryActionState"), "warning");
        assert!(!c.boolean("updatesAvailable"));
    }
}
