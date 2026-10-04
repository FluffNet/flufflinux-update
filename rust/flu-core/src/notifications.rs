//! Session-owned native Plasma progress; never runs in the privileged worker.
//! The worker's state is read-only. Closing/detaching a job view does not cancel
//! Pacman. Panel presence belongs to a D-Bus connection, so crashes/unloads work
//! without a final QML signal and multiple open panels are handled correctly.
use crate::{
    desktop::{download_time_remaining, human_size},
    runtime::{STATE, read_json},
};
use serde_json::Value;
use std::{
    collections::HashMap,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};
use zbus::{
    blocking::{Connection, Proxy, connection::Builder},
    message::Header,
    zvariant::{OwnedObjectPath, OwnedValue, Str},
};

pub const SERVICE: &str = "org.flufflinux.Update.Notifications";
pub const PATH: &str = "/org/flufflinux/Update/Notifications";
const JOB_SERVICE: &str = "org.kde.kuiserver";
const DESKTOP_ENTRY: &str = "org.flufflinux.update.worker";
const TIMEOUT: Duration = Duration::from_secs(2);
pub type Strings = HashMap<String, String>;
type Properties = HashMap<String, OwnedValue>;

// Reuse the exact panel translations, including operational failures.
pub const MESSAGES: &[&str] = &[
    "Fluff Linux Update",
    "Install Updates",
    "Downloading updates…",
    "Installing updates…",
    "%1 / %2 downloaded",
    "%1/%2 updates installed",
    "Estimated time: %1",
    "System updates were installed successfully.",
    "The system update failed.",
    "Update process was cancelled",
    "The update process could not be started.",
    "Connection failed while downloading updates. Check your network and try again.",
    "Fluff Linux Update could not verify the FluffNet repository signing key. The update was stopped to protect your system.",
    "pacman process is already running.",
];

pub fn messages() -> impl Iterator<Item = &'static str> {
    MESSAGES
        .iter()
        .chain(crate::operational_error::MESSAGES)
        .copied()
}

fn text(strings: &Strings, key: &str) -> String {
    strings
        .get(key)
        .filter(|s| !s.is_empty())
        .cloned()
        .unwrap_or_else(|| key.into())
}
fn format(strings: &Strings, key: &str, args: &[String]) -> String {
    let mut result = text(strings, key);
    for (i, arg) in args.iter().enumerate().rev() {
        result = result.replace(&format!("%{}", i + 1), &format!("\u{2066}{arg}\u{2069}"));
    }
    result
}
fn number(state: &Value, key: &str) -> u64 {
    state[key].as_u64().unwrap_or(0)
}
fn string(value: impl AsRef<str>) -> OwnedValue {
    OwnedValue::from(Str::from(value.as_ref()))
}
fn body_markup(value: &str) -> String {
    // Plasma treats job text as rich text: a plain newline is collapsed.
    // Escape all text first, then insert only the explicit line breaks.
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('\n', "<br/>")
}
fn active(state: &Value) -> bool {
    matches!(
        state["phase"].as_str(),
        Some("starting" | "downloading" | "installing")
    )
}
fn operation(state: &Value) -> String {
    state["operation_id"]
        .as_str()
        .map(str::to_owned)
        .unwrap_or_else(|| format!("legacy-{}", number(state, "worker_pid")))
}

/// Native JobViewV3 properties use the same per-phase percent/bytes/counts as
/// QML. Never estimate installation time or retain download speed in install.
pub fn progress_properties(state: &Value, strings: &Strings) -> Properties {
    let downloading = state["phase"] == "downloading";
    let installing = state["phase"] == "installing";
    let title = if downloading {
        "Downloading updates…"
    } else if installing {
        "Installing updates…"
    } else {
        "Install Updates"
    };
    let total = number(state, "total_download_bytes");
    let received = number(state, "downloaded_bytes").min(total);
    let speed = if downloading {
        number(state, "download_speed_bytes")
    } else {
        0
    };
    let body = if downloading {
        let mut body = format(
            strings,
            "%1 / %2 downloaded",
            &[human_size(received as i64), human_size(total as i64)],
        );
        if let Some(rate) = state["speed"].as_str().filter(|s| !s.is_empty()) {
            body.push_str(&format!(" · \u{2066}{rate}\u{2069}"));
        }
        if let Some(duration) = download_time_remaining(state) {
            body.push('\n');
            body.push_str(&format(strings, "Estimated time: %1", &[duration]));
        }
        body
    } else if installing {
        format(
            strings,
            "%1/%2 updates installed",
            &[
                number(state, "completed_packages").to_string(),
                number(state, "total_packages").to_string(),
            ],
        )
    } else {
        String::new()
    };
    HashMap::from([
        ("title".into(), string(text(strings, title))),
        ("infoMessage".into(), string(body_markup(&body))),
        (
            "percent".into(),
            (state["progress"]
                .as_f64()
                .unwrap_or(0.)
                .clamp(0., 100.)
                .floor() as u32)
                .into(),
        ),
        (
            "totalBytes".into(),
            (if downloading { total } else { 0 }).into(),
        ),
        (
            "processedBytes".into(),
            (if downloading { received } else { 0 }).into(),
        ),
        ("speed".into(), speed.into()),
        (
            "elapsedTime".into(),
            (if downloading {
                number(state, "download_elapsed_ms").min(i64::MAX as u64) as i64
            } else {
                0
            })
            .into(),
        ),
    ])
}

#[derive(Default)]
pub struct Lifecycle {
    observed: Option<String>,
    finished: Option<String>,
    last_operation: Option<String>,
}
#[derive(Debug, PartialEq)]
pub enum Display {
    Hidden,
    Progress,
    Finished,
}
impl Lifecycle {
    pub fn display(&mut self, state: &Value, visible: bool) -> Display {
        let id = operation(state);
        // A cached/very small transaction can start and finish between two
        // observer ticks. Its new operation ID still warrants one result;
        // the first snapshot of an old terminal state never does.
        let new_operation = self.last_operation.as_ref().is_some_and(|old| old != &id);
        if state["phase"].is_string() || self.last_operation.is_none() {
            self.last_operation = Some(id.clone());
        }
        if active(state) {
            if self.observed.as_ref() != Some(&id) {
                self.observed = Some(id);
                self.finished = None;
            }
            return if visible {
                Display::Hidden
            } else {
                Display::Progress
            };
        }
        if matches!(
            state["phase"].as_str(),
            Some("complete" | "failed" | "cancelled")
        ) && (self.observed.as_ref() == Some(&id) || new_operation)
            && self.finished.as_ref() != Some(&id)
        {
            self.finished = Some(id);
            return if visible {
                Display::Hidden
            } else {
                Display::Finished
            };
        }
        Display::Hidden
    }
}

#[derive(Default)]
struct Presence {
    panels: HashMap<String, bool>,
    strings: Strings,
}
struct PanelService(Arc<Mutex<Presence>>);
#[zbus::interface(name = "org.flufflinux.Update.Notifications")]
impl PanelService {
    fn set_visible(&self, visible: bool, strings: Strings, #[zbus(header)] header: Header<'_>) {
        if let Some(sender) = header.sender() {
            let mut presence = self.0.lock().unwrap();
            presence.panels.insert(sender.to_string(), visible);
            // Only known UI strings, bounded and supplied by this user's UI.
            for key in MESSAGES {
                if let Some(value) = strings.get(*key).filter(|v| v.len() < 8192) {
                    presence.strings.insert((*key).into(), value.clone());
                }
            }
        }
    }
}

/// One unique session-bus connection per QObject. Work is never done on Qt's
/// GUI thread, and bus/notification failures cannot prevent an update.
pub fn watch_panel(visible: Arc<AtomicBool>, stop: Arc<AtomicBool>, strings: Strings) {
    std::thread::spawn(move || {
        let mut connection = None;
        let mut previous = None;
        let mut heartbeat = 0;
        while !stop.load(Ordering::Relaxed) {
            let showing = visible.load(Ordering::Relaxed);
            if previous != Some(showing) || heartbeat == 0 {
                if connection.is_none() {
                    connection = Builder::session()
                        .and_then(|b| b.method_timeout(TIMEOUT).build())
                        .ok();
                }
                let result = connection.as_ref().map(|bus: &Connection| {
                    bus.call_method(
                        Some(SERVICE),
                        PATH,
                        Some(SERVICE),
                        "SetVisible",
                        &(showing, &strings),
                    )
                });
                if result.is_some_and(|r| r.is_ok()) {
                    previous = Some(showing);
                } else {
                    connection = None;
                    previous = None;
                }
            }
            heartbeat = (heartbeat + 1) % 8;
            std::thread::sleep(Duration::from_millis(250));
        }
        // Dropping the connection releases presence even if QML was destroyed
        // without delivering an onVisibleChanged/onDestruction callback.
    });
}

fn owner(bus: &Connection, name: &str) -> zbus::Result<String> {
    bus.call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "GetNameOwner",
        &(name,),
    )?
    .body()
    .deserialize()
}

fn owns_service(bus: &Connection) -> bool {
    bus.unique_name()
        .is_some_and(|name| owner(bus, SERVICE).is_ok_and(|owner| owner == name.as_str()))
}

#[derive(Default)]
struct Presentation {
    job: Option<OwnedObjectPath>,
    job_owner: String,
    notification: Option<u32>,
    operation: String,
}
impl Presentation {
    fn detach(&mut self, bus: &Connection) -> zbus::Result<()> {
        if let Some(path) = &self.job {
            // KJob::KilledJobError (1) discards only the UI proxy, exactly like
            // App Center unregistering its job when the window is reopened.
            Proxy::new(bus, JOB_SERVICE, path.as_str(), "org.kde.JobViewV3")?
                .call_noreply("terminate", &(1_u32, "", Properties::new()))?;
            self.job = None;
        }
        Ok(())
    }
    fn clear_finished(&mut self, bus: &Connection) -> zbus::Result<()> {
        if let Some(id) = self.notification {
            bus.call_method(
                Some("org.freedesktop.Notifications"),
                "/org/freedesktop/Notifications",
                Some("org.freedesktop.Notifications"),
                "CloseNotification",
                &(id,),
            )?;
            self.notification = None;
        }
        Ok(())
    }
    fn progress(&mut self, bus: &Connection, state: &Value, strings: &Strings) -> zbus::Result<()> {
        self.clear_finished(bus)?;
        let server = owner(bus, JOB_SERVICE)?;
        if self.job_owner != server {
            self.job = None;
            self.job_owner = server;
        }
        let id = operation(state);
        if self.operation != id {
            self.detach(bus)?;
            self.operation = id;
        }
        let mut properties = progress_properties(state, strings);
        if self.job.is_none() {
            properties.insert("immediate".into(), true.into());
            self.job = Some(
                bus.call_method(
                    Some(JOB_SERVICE),
                    "/JobViewServer",
                    Some("org.kde.JobViewServerV2"),
                    "requestView",
                    &(DESKTOP_ENTRY, 0_i32, &properties),
                )?
                .body()
                .deserialize()?,
            );
        }
        Proxy::new(
            bus,
            JOB_SERVICE,
            self.job.as_ref().unwrap().as_str(),
            "org.kde.JobViewV3",
        )?
        .call_noreply("update", &(properties,))
    }
    fn finished(&mut self, bus: &Connection, state: &Value, strings: &Strings) -> zbus::Result<()> {
        self.detach(bus)?;
        let message = match state["phase"].as_str() {
            Some("complete") => "System updates were installed successfully.",
            Some("cancelled") => "Update process was cancelled",
            _ => crate::operational_error::message(state["error"].as_str().unwrap_or("")),
        };
        // Plasma uses Notify's app_icon as the themed message-side icon.
        // desktop-entry supplies FLU's separate name/update icon in the header.
        // An image-path theme name would be overridden by app_icon in Plasma.
        let status_icon = match state["phase"].as_str() {
            Some("complete") => "flufflinux-update-success",
            Some("cancelled") => "dialog-information",
            _ => "flufflinux-update-error",
        };
        let hints = Properties::from([
            ("desktop-entry".into(), string(DESKTOP_ENTRY)),
            ("urgency".into(), 1_u8.into()),
        ]);
        // A normal completion notification can be withdrawn on reopen; a
        // terminated Plasma job-history entry cannot reliably be withdrawn.
        let body = body_markup(&text(strings, message));
        self.notification = Some(
            bus.call_method(
                Some("org.freedesktop.Notifications"),
                "/org/freedesktop/Notifications",
                Some("org.freedesktop.Notifications"),
                "Notify",
                &(
                    text(strings, "Fluff Linux Update"),
                    self.notification.unwrap_or(0),
                    status_icon,
                    text(strings, "Fluff Linux Update"),
                    body,
                    Vec::<String>::new(),
                    hints,
                    -1_i32,
                ),
            )?
            .body()
            .deserialize()?,
        );
        Ok(())
    }
}

/// D-Bus activation keeps this lightweight user-session observer independent
/// of kcmshell6/System Settings. It never executes package-manager commands.
pub fn run() -> zbus::Result<()> {
    let presence = Arc::new(Mutex::new(Presence::default()));
    let bus = Builder::session()?
        .method_timeout(TIMEOUT)
        .allow_name_replacements(false)
        .replace_existing_names(false)
        .name(SERVICE)?
        .serve_at(PATH, PanelService(presence.clone()))?
        .build()?;
    let mut lifecycle = Lifecycle::default();
    let mut presentation = Presentation::default();
    let mut pending_finish = None;
    loop {
        // Only the name's actual owner may publish. Duplicate launches cannot
        // replace the active observer; logout/name loss ends it cleanly.
        if !owns_service(&bus) {
            break;
        }
        // A disconnected UI no longer suppresses progress, including a crash,
        // module unload, multiple windows or a System Settings page change.
        let peers: Vec<String> = presence.lock().unwrap().panels.keys().cloned().collect();
        for peer in peers {
            if owner(&bus, &peer).is_err() {
                presence.lock().unwrap().panels.remove(&peer);
            }
        }
        let (visible, strings) = {
            let p = presence.lock().unwrap();
            (p.panels.values().any(|v| *v), p.strings.clone())
        };
        let state = read_json(STATE);
        let display = lifecycle.display(&state, visible);
        if visible {
            pending_finish = None;
        }
        if display == Display::Finished {
            pending_finish = Some(state.clone());
        }
        let result = if visible {
            presentation
                .detach(&bus)
                .and_then(|()| presentation.clear_finished(&bus))
        } else if display == Display::Progress {
            pending_finish = None;
            presentation.progress(&bus, &state, &strings)
        } else if let Some(ref finished) = pending_finish {
            let result = presentation.finished(&bus, finished, &strings);
            if result.is_ok() {
                pending_finish = None;
            }
            result
        } else {
            presentation.detach(&bus)
        };
        if let Err(error) = result {
            eprintln!("FLU notification delivery: {error}");
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    Ok(())
}

#[cfg(test)]
#[path = "notifications_tests.rs"]
mod tests;
