use super::*;
use serde_json::json;

fn state(phase: &str) -> Value {
    json!({"operation_id":"test-1", "phase":phase, "progress":37.5,
        "total_download_bytes":1000, "downloaded_bytes":375,
        "download_speed_bytes":125, "speed":"125 B/s", "download_elapsed_ms":3000,
        "completed_packages":3, "total_packages":8})
}

#[test]
fn close_reopen_close_only_changes_presentation() {
    let mut life = Lifecycle::default();
    let downloading = state("downloading");
    for (visible, expected) in [
        (true, Display::Hidden),
        (false, Display::Progress),
        (true, Display::Hidden),
        (false, Display::Progress),
    ] {
        assert_eq!(life.display(&downloading, visible), expected);
    }
    assert_eq!(life.display(&state("installing"), false), Display::Progress);
    assert_eq!(life.display(&state("complete"), false), Display::Finished);
    assert_eq!(life.display(&state("complete"), false), Display::Hidden);
    assert_eq!(life.display(&state("complete"), true), Display::Hidden);
    assert_eq!(life.display(&state("complete"), false), Display::Hidden);
}

#[test]
fn historical_and_foreground_completions_never_reappear() {
    for phase in ["complete", "failed", "cancelled"] {
        let mut life = Lifecycle::default();
        assert_eq!(life.display(&state(phase), false), Display::Hidden);
        assert_eq!(life.display(&state("installing"), true), Display::Hidden);
        assert_eq!(life.display(&state(phase), true), Display::Hidden);
        assert_eq!(life.display(&state(phase), false), Display::Hidden);
        let mut next = state("starting");
        next["operation_id"] = json!("test-2");
        assert_eq!(life.display(&next, false), Display::Progress);
        next["phase"] = json!(phase);
        assert_eq!(life.display(&next, false), Display::Finished);
    }
}

#[test]
fn a_fast_transaction_still_reports_once_without_replaying_history() {
    for terminal in ["complete", "failed", "cancelled"] {
        for visible in [false, true] {
            let mut life = Lifecycle::default();
            assert_eq!(life.display(&state("complete"), false), Display::Hidden);
            assert_eq!(life.display(&json!({}), false), Display::Hidden);
            assert_eq!(life.display(&state("complete"), false), Display::Hidden);
            let mut next = state(terminal);
            next["operation_id"] = json!("completed-between-ticks");
            assert_eq!(
                life.display(&next, visible),
                if visible {
                    Display::Hidden
                } else {
                    Display::Finished
                }
            );
            assert_eq!(life.display(&next, false), Display::Hidden);
        }
    }
}

#[test]
fn download_matches_qml_and_estimates_only_with_real_speed() {
    let mut data = state("downloading");
    let properties = progress_properties(&data, &Strings::new());
    assert_eq!(u32::try_from(&properties["percent"]).unwrap(), 37);
    assert_eq!(u64::try_from(&properties["processedBytes"]).unwrap(), 375);
    assert_eq!(u64::try_from(&properties["speed"]).unwrap(), 125);
    let body = <&str>::try_from(&properties["infoMessage"]).unwrap();
    assert!(
        body.contains("125 B/s") && body.contains("<br/>Estimated time: \u{2066}0:00:05\u{2069}")
    );
    data["download_speed_bytes"] = json!(0);
    let properties = progress_properties(&data, &Strings::new());
    assert!(
        !<&str>::try_from(&properties["infoMessage"])
            .unwrap()
            .contains("Estimated time:")
    );
    data["download_speed_bytes"] = json!(125);
    data["phase"] = json!("installing");
    let properties = progress_properties(&data, &Strings::new());
    assert_eq!(u64::try_from(&properties["speed"]).unwrap(), 0);
    assert_eq!(u64::try_from(&properties["totalBytes"]).unwrap(), 0);
    assert_eq!(
        <&str>::try_from(&properties["title"]).unwrap(),
        "Installing updates…"
    );
    assert!(
        <&str>::try_from(&properties["infoMessage"])
            .unwrap()
            .contains("updates installed")
    );
}

#[test]
fn translated_eta_keeps_technical_duration_ltr() {
    let strings = Strings::from([("Estimated time: %1".into(), "זמן משוער: %1".into())]);
    let properties = progress_properties(&state("downloading"), &strings);
    assert!(
        <&str>::try_from(&properties["infoMessage"])
            .unwrap()
            .contains("<br/>זמן משוער: \u{2066}0:00:05\u{2069}")
    );
}

#[cfg(target_os = "linux")]
mod wire {
    use super::*;
    use std::{
        io::{BufRead, BufReader},
        process::{Child, Command, Stdio},
    };
    struct PrivateBus(Child);
    impl Drop for PrivateBus {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    type Calls = Arc<Mutex<Vec<String>>>;
    struct JobServer(Calls);
    #[zbus::interface(name = "org.kde.JobViewServerV2")]
    impl JobServer {
        #[zbus(name = "requestView")]
        fn request_view(
            &self,
            desktop: &str,
            capabilities: i32,
            hints: Properties,
        ) -> OwnedObjectPath {
            assert_eq!(desktop, DESKTOP_ENTRY);
            assert_eq!(capabilities, 0); // No unsafe install-cancellation control.
            assert!(bool::try_from(&hints["immediate"]).unwrap());
            self.0.lock().unwrap().push("start".into());
            OwnedObjectPath::try_from("/JobViewServer/JobView_1").unwrap()
        }
    }
    struct JobView(Calls);
    #[zbus::interface(name = "org.kde.JobViewV3")]
    impl JobView {
        #[zbus(name = "update")]
        fn update(&self, properties: Properties) {
            self.0.lock().unwrap().push(format!(
                "progress:{}",
                u32::try_from(&properties["percent"]).unwrap()
            ));
        }
        #[zbus(name = "terminate")]
        fn terminate(&self, error: u32, _message: &str, _hints: Properties) {
            assert_eq!(error, 1); // Reopening must not emit false success.
            self.0.lock().unwrap().push("detach".into());
        }
    }
    struct Notifications(Calls);
    #[zbus::interface(name = "org.freedesktop.Notifications")]
    impl Notifications {
        #[allow(clippy::too_many_arguments)]
        fn notify(
            &self,
            _app: &str,
            _id: u32,
            icon: &str,
            _summary: &str,
            body: &str,
            _actions: Vec<String>,
            hints: Properties,
            _timeout: i32,
        ) -> u32 {
            assert_eq!(
                <&str>::try_from(&hints["desktop-entry"]).unwrap(),
                DESKTOP_ENTRY
            );
            self.0.lock().unwrap().push(format!("status-icon:{icon}"));
            self.0.lock().unwrap().push(format!("notice:{body}"));
            42
        }
        fn close_notification(&self, id: u32) {
            assert_eq!(id, 42);
            self.0.lock().unwrap().push("close-notice".into());
        }
    }
    #[test]
    fn native_protocol_detach_recreate_finish_close_and_multiple_panels() {
        let mut private = PrivateBus(
            Command::new("dbus-daemon")
                .args(["--session", "--nofork", "--print-address=1"])
                .stdout(Stdio::piped())
                .spawn()
                .unwrap(),
        );
        let mut address = String::new();
        BufReader::new(private.0.stdout.take().unwrap())
            .read_line(&mut address)
            .unwrap();
        let address = address.trim();
        let calls: Calls = Arc::default();
        let presence = Arc::new(Mutex::new(Presence::default()));
        let _service = Builder::address(address)
            .unwrap()
            .allow_name_replacements(false)
            .replace_existing_names(false)
            .name(JOB_SERVICE)
            .unwrap()
            .name("org.freedesktop.Notifications")
            .unwrap()
            .name(SERVICE)
            .unwrap()
            .serve_at("/JobViewServer", JobServer(calls.clone()))
            .unwrap()
            .serve_at("/JobViewServer/JobView_1", JobView(calls.clone()))
            .unwrap()
            .serve_at(
                "/org/freedesktop/Notifications",
                Notifications(calls.clone()),
            )
            .unwrap()
            .serve_at(PATH, PanelService(presence.clone()))
            .unwrap()
            .build()
            .unwrap();
        assert!(owns_service(&_service));
        let duplicate = Builder::address(address)
            .unwrap()
            .allow_name_replacements(false)
            .replace_existing_names(false)
            .name(SERVICE)
            .unwrap()
            .build();
        assert!(duplicate.is_err());
        assert!(owns_service(&_service));
        let client = Builder::address(address)
            .unwrap()
            .method_timeout(TIMEOUT)
            .build()
            .unwrap();
        let second = Builder::address(address)
            .unwrap()
            .method_timeout(TIMEOUT)
            .build()
            .unwrap();
        for (c, visible) in [(&client, false), (&second, true)] {
            c.call_method(
                Some(SERVICE),
                PATH,
                Some(SERVICE),
                "SetVisible",
                &(visible, Strings::new()),
            )
            .unwrap();
        }
        assert!(presence.lock().unwrap().panels.values().any(|v| *v));
        let second_name = second.unique_name().unwrap().to_string();
        drop(second);
        for _ in 0..100 {
            if owner(&client, &second_name).is_err() {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(owner(&client, &second_name).is_err());
        presence.lock().unwrap().panels.remove(&second_name);
        assert!(!presence.lock().unwrap().panels.values().any(|v| *v));
        let mut ui = Presentation::default();
        let strings = Strings::new();
        ui.progress(&client, &state("downloading"), &strings)
            .unwrap();
        ui.detach(&client).unwrap();
        ui.progress(&client, &state("installing"), &strings)
            .unwrap();
        ui.finished(&client, &state("complete"), &strings).unwrap();
        ui.clear_finished(&client).unwrap();
        let mut failed = state("failed");
        failed["error"] = json!("DOWNLOAD_CONNECTION_FAILED");
        ui.finished(&client, &failed, &strings).unwrap();
        ui.clear_finished(&client).unwrap();
        for _ in 0..100 {
            if calls
                .lock()
                .unwrap()
                .last()
                .is_some_and(|s| s == "close-notice")
            {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let result = calls.lock().unwrap();
        assert_eq!(result.iter().filter(|s| *s == "start").count(), 2);
        assert_eq!(result.iter().filter(|s| *s == "detach").count(), 2);
        assert_eq!(
            result.iter().filter(|s| s.starts_with("notice:")).count(),
            2
        );
        assert!(
            result
                .iter()
                .any(|s| s == "notice:System updates were installed successfully.")
        );
        assert!(
            result
                .iter()
                .any(|s| s.starts_with("notice:Connection failed while downloading"))
        );
        assert!(
            result
                .iter()
                .any(|s| s == "status-icon:flufflinux-update-success")
        );
        assert!(
            result
                .iter()
                .any(|s| s == "status-icon:flufflinux-update-error")
        );
        assert_eq!(result.iter().filter(|s| *s == "close-notice").count(), 2);
        assert_eq!(result.last().unwrap(), "close-notice");
        _service.release_name(SERVICE).unwrap();
        assert!(!owns_service(&_service));
    }
}
