//! Rust-owned QObject boundary. The existing QML reads these native properties.
use cxx_qt::{CxxQtType, Threading};
use cxx_qt_lib::{QList, QString, QVariant};
use flu_core::desktop::{self, Command};
use flu_core::notifications;
use std::{
    path::PathBuf,
    pin::Pin,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::Sender,
    },
};

#[cxx_qt::bridge(namespace = "flu")]
mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        #[namespace = ""]
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qvariant.h");
        #[namespace = ""]
        type QVariant = cxx_qt_lib::QVariant;
        include!("cxx-qt-lib/core/qlist/qlist_QVariant.h");
        #[namespace = ""]
        type QList_QVariant = cxx_qt_lib::QList<cxx_qt_lib::QVariant>;
        include!("nativeqt.h");
        fn translate(message: &str, plural: &str, count: i64, arguments: &Vec<String>) -> String;
        fn config_directory() -> String;
        fn initialize_locale(languages: &Vec<String>);
        fn variants(json: &str) -> QList_QVariant;
        fn copy_text(text: &QString);
        fn open_url(url: &str);
        fn reachability() -> i32;
        fn interface_flags() -> Vec<u32>;
    }
    extern "RustQt" {
        #[qobject]
        #[qproperty(QString, last_update, cxx_name = "lastUpdate", READ, NOTIFY)]
        #[qproperty(bool, has_last_update, cxx_name = "hasLastUpdate", READ, NOTIFY)]
        #[qproperty(QString, state_message, cxx_name = "stateMessage", READ, NOTIFY)]
        #[qproperty(QString, freshness_text, cxx_name = "freshnessText", READ, NOTIFY)]
        #[qproperty(QString, freshness_color, cxx_name = "freshnessColor", READ, NOTIFY)]
        #[qproperty(QString, relative_time, cxx_name = "relativeTime", READ, NOTIFY)]
        #[qproperty(bool, checking, cxx_name = "checking", READ, NOTIFY)]
        #[qproperty(bool, check_complete, cxx_name = "checkComplete", READ, NOTIFY)]
        #[qproperty(bool, updates_available, cxx_name = "updatesAvailable", READ, NOTIFY)]
        #[qproperty(QString, download_size, cxx_name = "downloadSize", READ, NOTIFY)]
        #[qproperty(QString, disk_change, cxx_name = "diskChange", READ, NOTIFY)]
        #[qproperty(bool, disk_space_freed, cxx_name = "diskSpaceFreed", READ, NOTIFY)]
        #[qproperty(QString, check_error, cxx_name = "checkError", READ, NOTIFY)]
        #[qproperty(
            QList_QVariant,
            update_packages,
            cxx_name = "updatePackages",
            READ,
            NOTIFY
        )]
        #[qproperty(bool, battery_low, cxx_name = "batteryLow", READ, NOTIFY)]
        #[qproperty(QString, install_phase, cxx_name = "installPhase", READ, NOTIFY)]
        #[qproperty(bool, update_active, cxx_name = "updateActive", READ, NOTIFY)]
        #[qproperty(f64, install_progress, cxx_name = "installProgress", READ, NOTIFY)]
        #[qproperty(i32, completed_packages, cxx_name = "completedPackages", READ, NOTIFY)]
        #[qproperty(i32, total_packages, cxx_name = "totalPackages", READ, NOTIFY)]
        #[qproperty(QString, downloaded_size, cxx_name = "downloadedSize", READ, NOTIFY)]
        #[qproperty(
            QString,
            total_download_size,
            cxx_name = "totalDownloadSize",
            READ,
            NOTIFY
        )]
        #[qproperty(QString, download_speed, cxx_name = "downloadSpeed", READ, NOTIFY)]
        #[qproperty(
            QString,
            download_time_remaining,
            cxx_name = "downloadTimeRemaining",
            READ,
            NOTIFY
        )]
        #[qproperty(QString, install_error, cxx_name = "installError", READ, NOTIFY)]
        #[qproperty(
            bool,
            signing_key_security_error,
            cxx_name = "signingKeySecurityError",
            READ,
            NOTIFY
        )]
        #[qproperty(
            QString,
            signing_key_technical_details,
            cxx_name = "signingKeyTechnicalDetails",
            READ,
            NOTIFY
        )]
        #[qproperty(
            QString,
            signing_key_issue_url,
            cxx_name = "signingKeyIssueUrl",
            READ,
            NOTIFY
        )]
        #[qproperty(
            bool,
            cancellation_notice,
            cxx_name = "cancellationNotice",
            READ,
            NOTIFY
        )]
        #[qproperty(
            bool,
            installation_success_notice,
            cxx_name = "installationSuccessNotice",
            READ,
            NOTIFY
        )]
        #[qproperty(bool, network_connected, cxx_name = "networkConnected", READ, NOTIFY)]
        #[qproperty(bool, network_limited, cxx_name = "networkLimited", READ, NOTIFY)]
        #[qproperty(
            QString,
            recovery_dialog_type,
            cxx_name = "recoveryDialogType",
            READ,
            NOTIFY
        )]
        #[qproperty(QString, recovery_package, cxx_name = "recoveryPackage", READ, NOTIFY)]
        #[qproperty(QString, recovery_notice, cxx_name = "recoveryNotice", READ, NOTIFY)]
        #[qproperty(
            QString,
            recovery_action_state,
            cxx_name = "recoveryActionState",
            READ,
            NOTIFY
        )]
        #[qproperty(bool, pacman_view, cxx_name = "pacmanView", READ, WRITE = set_pacman_view, NOTIFY)]
        #[qproperty(i32, update_window_width, cxx_name = "updateWindowWidth", READ, NOTIFY)]
        #[qproperty(
            i32,
            update_window_height,
            cxx_name = "updateWindowHeight",
            READ,
            NOTIFY
        )]
        #[qproperty(
            bool,
            update_window_maximized,
            cxx_name = "updateWindowMaximized",
            READ,
            NOTIFY
        )]
        type UpdateBackend = super::UpdateBackendRust;
        #[qinvokable]
        #[cxx_name = "setPanelVisible"]
        fn set_panel_visible(self: Pin<&mut UpdateBackend>, visible: bool);
        #[qinvokable]
        #[cxx_name = "refresh"]
        fn refresh(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "checkForUpdates"]
        fn check_for_updates(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "clearCheckResult"]
        fn clear_check_result(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "startInstallation"]
        fn start_installation(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "cancelInstallation"]
        fn cancel_installation(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "retrySigningKeyUpdate"]
        fn retry_signing_key_update(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "resolveRemovalWarning"]
        fn resolve_removal_warning(self: Pin<&mut UpdateBackend>, allow: bool);
        #[qinvokable]
        #[cxx_name = "setPacmanView"]
        fn set_pacman_view(self: Pin<&mut UpdateBackend>, enabled: bool);
        #[qinvokable]
        #[cxx_name = "saveUpdateWindowState"]
        fn save_update_window_state(
            self: Pin<&mut UpdateBackend>,
            width: i32,
            height: i32,
            maximized: bool,
        );
        #[qinvokable]
        #[cxx_name = "copySigningKeyTechnicalDetails"]
        fn copy_signing_key_technical_details(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "openSigningKeyIssue"]
        fn open_signing_key_issue(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "pollNetwork"]
        fn poll_network(self: Pin<&mut UpdateBackend>);
        #[qinvokable]
        #[cxx_name = "hostTitle"]
        fn host_title(self: &UpdateBackend) -> QString;
        #[qsignal]
        #[cxx_name = "installStateChanged"]
        fn install_state_changed(self: Pin<&mut UpdateBackend>);
    }
    impl cxx_qt::Threading for UpdateBackend {}
    impl cxx_qt::Initialize for UpdateBackend {}
}
pub struct UpdateBackendRust {
    last_update: QString,
    has_last_update: bool,
    state_message: QString,
    freshness_text: QString,
    freshness_color: QString,
    relative_time: QString,
    checking: bool,
    check_complete: bool,
    updates_available: bool,
    download_size: QString,
    disk_change: QString,
    disk_space_freed: bool,
    check_error: QString,
    update_packages: QList<QVariant>,
    battery_low: bool,
    install_phase: QString,
    update_active: bool,
    install_progress: f64,
    completed_packages: i32,
    total_packages: i32,
    downloaded_size: QString,
    total_download_size: QString,
    download_speed: QString,
    download_time_remaining: QString,
    install_error: QString,
    signing_key_security_error: bool,
    signing_key_technical_details: QString,
    signing_key_issue_url: QString,
    cancellation_notice: bool,
    installation_success_notice: bool,
    network_connected: bool,
    network_limited: bool,
    recovery_dialog_type: QString,
    recovery_package: QString,
    recovery_notice: QString,
    recovery_action_state: QString,
    pacman_view: bool,
    update_window_width: i32,
    update_window_height: i32,
    update_window_maximized: bool,
    sender: Option<Sender<Command>>,
    stop: Arc<AtomicBool>,
    panel_visible: Arc<AtomicBool>,
    settings: PathBuf,
}
impl Default for UpdateBackendRust {
    fn default() -> Self {
        Self {
            last_update: Default::default(),
            has_last_update: false,
            state_message: Default::default(),
            freshness_text: Default::default(),
            freshness_color: Default::default(),
            relative_time: Default::default(),
            checking: false,
            check_complete: false,
            updates_available: false,
            download_size: Default::default(),
            disk_change: Default::default(),
            disk_space_freed: false,
            check_error: Default::default(),
            update_packages: Default::default(),
            battery_low: false,
            install_phase: Default::default(),
            update_active: false,
            install_progress: 0.,
            completed_packages: 0,
            total_packages: 0,
            downloaded_size: Default::default(),
            total_download_size: Default::default(),
            download_speed: Default::default(),
            download_time_remaining: Default::default(),
            install_error: Default::default(),
            signing_key_security_error: false,
            signing_key_technical_details: Default::default(),
            signing_key_issue_url: Default::default(),
            cancellation_notice: false,
            installation_success_notice: false,
            network_connected: false,
            network_limited: false,
            recovery_dialog_type: Default::default(),
            recovery_package: Default::default(),
            recovery_notice: Default::default(),
            recovery_action_state: Default::default(),
            pacman_view: false,
            update_window_width: 0,
            update_window_height: 0,
            update_window_maximized: false,
            sender: None,
            stop: Arc::new(AtomicBool::new(false)),
            panel_visible: Arc::new(AtomicBool::new(true)),
            settings: PathBuf::new(),
        }
    }
}
impl Drop for UpdateBackendRust {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}
impl cxx_qt::Initialize for ffi::UpdateBackend {
    fn initialize(mut self: Pin<&mut Self>) {
        let directory = PathBuf::from(ffi::config_directory());
        let plasma = desktop::parse_ini(&directory.join("plasma-localerc"));
        let languages = plasma
            .get("Translations")
            .and_then(|v| v.get("LANGUAGE"))
            .map(|s| {
                s.split(':')
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .map(String::from)
                    .collect()
            })
            .unwrap_or_default();
        ffi::initialize_locale(&languages);
        let strings = notifications::messages()
            .map(|message| {
                let arguments = if message.contains("%2") {
                    vec!["%1".into(), "%2".into()]
                } else if message.contains("%1") {
                    vec!["%1".into()]
                } else {
                    vec![]
                };
                (message.into(), ffi::translate(message, "", -1, &arguments))
            })
            .collect();
        notifications::watch_panel(
            self.rust().panel_visible.clone(),
            self.rust().stop.clone(),
            strings,
        );
        let path = directory.join("flufflinux-update/settings.conf");
        let settings = desktop::parse_ini(&path);
        self.as_mut().update_pacman_view(
            settings
                .get("Interface")
                .and_then(|v| v.get("PacmanView"))
                .is_some_and(|s| s == "true" || s == "1"),
        );
        self.as_mut().set_update_window_width(
            settings
                .get("UpdateWindow")
                .and_then(|v| v.get("Width"))
                .and_then(|v| v.parse().ok())
                .unwrap_or(0),
        );
        self.as_mut().set_update_window_height(
            settings
                .get("UpdateWindow")
                .and_then(|v| v.get("Height"))
                .and_then(|v| v.parse().ok())
                .unwrap_or(0),
        );
        self.as_mut().set_update_window_maximized(
            settings
                .get("UpdateWindow")
                .and_then(|v| v.get("Maximized"))
                .is_some_and(|s| s == "true" || s == "1"),
        );
        self.as_mut().rust_mut().settings = path;
        self.as_mut().apply_snapshot(desktop::initial_model());
        let thread = self.qt_thread();
        let sender = desktop::start_controller(
            Arc::new(|s, p, n, a| ffi::translate(s, p, n, &a.to_vec())),
            move |snapshot| {
                let _ = thread.queue(move |object| object.apply_snapshot(snapshot));
            },
            self.rust().stop.clone(),
        );
        self.as_mut().rust_mut().sender = Some(sender);
        self.poll_network();
    }
}
impl ffi::UpdateBackend {
    fn set_panel_visible(self: Pin<&mut Self>, visible: bool) {
        self.rust().panel_visible.store(visible, Ordering::Relaxed);
    }
    fn set_last_update(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().last_update != value {
            self.as_mut().rust_mut().last_update = value;
            self.last_update_changed();
        }
    }
    fn set_has_last_update(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().has_last_update != value {
            self.as_mut().rust_mut().has_last_update = value;
            self.has_last_update_changed();
        }
    }
    fn set_state_message(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().state_message != value {
            self.as_mut().rust_mut().state_message = value;
            self.state_message_changed();
        }
    }
    fn set_freshness_text(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().freshness_text != value {
            self.as_mut().rust_mut().freshness_text = value;
            self.freshness_text_changed();
        }
    }
    fn set_freshness_color(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().freshness_color != value {
            self.as_mut().rust_mut().freshness_color = value;
            self.freshness_color_changed();
        }
    }
    fn set_relative_time(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().relative_time != value {
            self.as_mut().rust_mut().relative_time = value;
            self.relative_time_changed();
        }
    }
    fn set_checking(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().checking != value {
            self.as_mut().rust_mut().checking = value;
            self.checking_changed();
        }
    }
    fn set_check_complete(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().check_complete != value {
            self.as_mut().rust_mut().check_complete = value;
            self.check_complete_changed();
        }
    }
    fn set_updates_available(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().updates_available != value {
            self.as_mut().rust_mut().updates_available = value;
            self.updates_available_changed();
        }
    }
    fn set_download_size(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().download_size != value {
            self.as_mut().rust_mut().download_size = value;
            self.download_size_changed();
        }
    }
    fn set_disk_change(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().disk_change != value {
            self.as_mut().rust_mut().disk_change = value;
            self.disk_change_changed();
        }
    }
    fn set_disk_space_freed(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().disk_space_freed != value {
            self.as_mut().rust_mut().disk_space_freed = value;
            self.disk_space_freed_changed();
        }
    }
    fn set_check_error(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().check_error != value {
            self.as_mut().rust_mut().check_error = value;
            self.check_error_changed();
        }
    }
    fn set_update_packages(mut self: Pin<&mut Self>, value: QList<QVariant>) {
        if self.rust().update_packages != value {
            self.as_mut().rust_mut().update_packages = value;
            self.update_packages_changed();
        }
    }
    fn set_battery_low(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().battery_low != value {
            self.as_mut().rust_mut().battery_low = value;
            self.battery_low_changed();
        }
    }
    fn set_install_phase(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().install_phase != value {
            self.as_mut().rust_mut().install_phase = value;
            self.install_phase_changed();
        }
    }
    fn set_update_active(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().update_active != value {
            self.as_mut().rust_mut().update_active = value;
            self.update_active_changed();
        }
    }
    fn set_install_progress(mut self: Pin<&mut Self>, value: f64) {
        if self.rust().install_progress != value {
            self.as_mut().rust_mut().install_progress = value;
            self.install_progress_changed();
        }
    }
    fn set_completed_packages(mut self: Pin<&mut Self>, value: i32) {
        if self.rust().completed_packages != value {
            self.as_mut().rust_mut().completed_packages = value;
            self.completed_packages_changed();
        }
    }
    fn set_total_packages(mut self: Pin<&mut Self>, value: i32) {
        if self.rust().total_packages != value {
            self.as_mut().rust_mut().total_packages = value;
            self.total_packages_changed();
        }
    }
    fn set_downloaded_size(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().downloaded_size != value {
            self.as_mut().rust_mut().downloaded_size = value;
            self.downloaded_size_changed();
        }
    }
    fn set_total_download_size(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().total_download_size != value {
            self.as_mut().rust_mut().total_download_size = value;
            self.total_download_size_changed();
        }
    }
    fn set_download_speed(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().download_speed != value {
            self.as_mut().rust_mut().download_speed = value;
            self.download_speed_changed();
        }
    }
    fn set_download_time_remaining(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().download_time_remaining != value {
            self.as_mut().rust_mut().download_time_remaining = value;
            self.download_time_remaining_changed();
        }
    }
    fn set_install_error(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().install_error != value {
            self.as_mut().rust_mut().install_error = value;
            self.install_error_changed();
        }
    }
    fn set_signing_key_security_error(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().signing_key_security_error != value {
            self.as_mut().rust_mut().signing_key_security_error = value;
            self.signing_key_security_error_changed();
        }
    }
    fn set_signing_key_technical_details(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().signing_key_technical_details != value {
            self.as_mut().rust_mut().signing_key_technical_details = value;
            self.signing_key_technical_details_changed();
        }
    }
    fn set_signing_key_issue_url(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().signing_key_issue_url != value {
            self.as_mut().rust_mut().signing_key_issue_url = value;
            self.signing_key_issue_url_changed();
        }
    }
    fn set_cancellation_notice(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().cancellation_notice != value {
            self.as_mut().rust_mut().cancellation_notice = value;
            self.cancellation_notice_changed();
        }
    }
    fn set_installation_success_notice(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().installation_success_notice != value {
            self.as_mut().rust_mut().installation_success_notice = value;
            self.installation_success_notice_changed();
        }
    }
    fn set_network_connected(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().network_connected != value {
            self.as_mut().rust_mut().network_connected = value;
            self.network_connected_changed();
        }
    }
    fn set_network_limited(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().network_limited != value {
            self.as_mut().rust_mut().network_limited = value;
            self.network_limited_changed();
        }
    }
    fn set_recovery_dialog_type(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().recovery_dialog_type != value {
            self.as_mut().rust_mut().recovery_dialog_type = value;
            self.recovery_dialog_type_changed();
        }
    }
    fn set_recovery_package(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().recovery_package != value {
            self.as_mut().rust_mut().recovery_package = value;
            self.recovery_package_changed();
        }
    }
    fn set_recovery_notice(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().recovery_notice != value {
            self.as_mut().rust_mut().recovery_notice = value;
            self.recovery_notice_changed();
        }
    }
    fn set_recovery_action_state(mut self: Pin<&mut Self>, value: QString) {
        if self.rust().recovery_action_state != value {
            self.as_mut().rust_mut().recovery_action_state = value;
            self.recovery_action_state_changed();
        }
    }
    fn set_update_window_width(mut self: Pin<&mut Self>, value: i32) {
        if self.rust().update_window_width != value {
            self.as_mut().rust_mut().update_window_width = value;
            self.update_window_width_changed();
        }
    }
    fn set_update_window_height(mut self: Pin<&mut Self>, value: i32) {
        if self.rust().update_window_height != value {
            self.as_mut().rust_mut().update_window_height = value;
            self.update_window_height_changed();
        }
    }
    fn set_update_window_maximized(mut self: Pin<&mut Self>, value: bool) {
        if self.rust().update_window_maximized != value {
            self.as_mut().rust_mut().update_window_maximized = value;
            self.update_window_maximized_changed();
        }
    }
    fn send(&self, command: Command) {
        if let Some(sender) = &self.rust().sender {
            let _ = sender.send(command);
        }
    }
    fn apply_snapshot(mut self: Pin<&mut Self>, snapshot: serde_json::Value) {
        let security_changed = self.rust().signing_key_security_error
            != snapshot["signingKeySecurityError"]
                .as_bool()
                .unwrap_or(false)
            || self.rust().install_phase.to_string()
                != snapshot["installPhase"].as_str().unwrap_or("");
        self.as_mut()
            .set_last_update(QString::from(snapshot["lastUpdate"].as_str().unwrap_or("")));
        self.as_mut()
            .set_has_last_update(snapshot["hasLastUpdate"].as_bool().unwrap_or(false));
        self.as_mut().set_state_message(QString::from(
            snapshot["stateMessage"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_freshness_text(QString::from(
            snapshot["freshnessText"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_freshness_color(QString::from(
            snapshot["freshnessColor"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_relative_time(QString::from(
            snapshot["relativeTime"].as_str().unwrap_or(""),
        ));
        self.as_mut()
            .set_checking(snapshot["checking"].as_bool().unwrap_or(false));
        self.as_mut()
            .set_check_complete(snapshot["checkComplete"].as_bool().unwrap_or(false));
        self.as_mut()
            .set_updates_available(snapshot["updatesAvailable"].as_bool().unwrap_or(false));
        self.as_mut().set_download_size(QString::from(
            snapshot["downloadSize"].as_str().unwrap_or(""),
        ));
        self.as_mut()
            .set_disk_change(QString::from(snapshot["diskChange"].as_str().unwrap_or("")));
        self.as_mut()
            .set_disk_space_freed(snapshot["diskSpaceFreed"].as_bool().unwrap_or(false));
        self.as_mut()
            .set_check_error(QString::from(snapshot["checkError"].as_str().unwrap_or("")));
        self.as_mut()
            .set_update_packages(ffi::variants(&snapshot["updatePackages"].to_string()));
        self.as_mut()
            .set_battery_low(snapshot["batteryLow"].as_bool().unwrap_or(false));
        self.as_mut().set_install_phase(QString::from(
            snapshot["installPhase"].as_str().unwrap_or(""),
        ));
        self.as_mut()
            .set_update_active(snapshot["updateActive"].as_bool().unwrap_or(false));
        self.as_mut()
            .set_install_progress(snapshot["installProgress"].as_f64().unwrap_or(0.));
        self.as_mut()
            .set_completed_packages(snapshot["completedPackages"].as_i64().unwrap_or(0) as i32);
        self.as_mut()
            .set_total_packages(snapshot["totalPackages"].as_i64().unwrap_or(0) as i32);
        self.as_mut().set_downloaded_size(QString::from(
            snapshot["downloadedSize"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_total_download_size(QString::from(
            snapshot["totalDownloadSize"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_download_speed(QString::from(
            snapshot["downloadSpeed"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_download_time_remaining(QString::from(
            snapshot["downloadTimeRemaining"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_install_error(QString::from(
            snapshot["installError"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_signing_key_security_error(
            snapshot["signingKeySecurityError"]
                .as_bool()
                .unwrap_or(false),
        );
        self.as_mut()
            .set_signing_key_technical_details(QString::from(
                snapshot["signingKeyTechnicalDetails"]
                    .as_str()
                    .unwrap_or(""),
            ));
        self.as_mut().set_signing_key_issue_url(QString::from(
            snapshot["signingKeyIssueUrl"].as_str().unwrap_or(""),
        ));
        self.as_mut()
            .set_cancellation_notice(snapshot["cancellationNotice"].as_bool().unwrap_or(false));
        self.as_mut().set_installation_success_notice(
            snapshot["installationSuccessNotice"]
                .as_bool()
                .unwrap_or(false),
        );
        self.as_mut()
            .set_network_connected(snapshot["networkConnected"].as_bool().unwrap_or(false));
        self.as_mut()
            .set_network_limited(snapshot["networkLimited"].as_bool().unwrap_or(false));
        self.as_mut().set_recovery_dialog_type(QString::from(
            snapshot["recoveryDialogType"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_recovery_package(QString::from(
            snapshot["recoveryPackage"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_recovery_notice(QString::from(
            snapshot["recoveryNotice"].as_str().unwrap_or(""),
        ));
        self.as_mut().set_recovery_action_state(QString::from(
            snapshot["recoveryActionState"].as_str().unwrap_or(""),
        ));
        if security_changed {
            self.as_mut().install_state_changed();
        }
    }
    fn refresh(self: Pin<&mut Self>) {
        self.send(Command::Refresh);
    }
    fn check_for_updates(self: Pin<&mut Self>) {
        if !self.rust().checking {
            self.send(Command::Check);
        }
    }
    fn clear_check_result(self: Pin<&mut Self>) {
        self.send(Command::Clear);
    }
    fn start_installation(self: Pin<&mut Self>) {
        self.send(Command::Install);
    }
    fn cancel_installation(self: Pin<&mut Self>) {
        self.send(Command::Cancel);
    }
    fn retry_signing_key_update(self: Pin<&mut Self>) {
        self.send(Command::RetryKey);
    }
    fn resolve_removal_warning(self: Pin<&mut Self>, allow: bool) {
        self.send(Command::Resolve(allow));
    }
    fn poll_network(self: Pin<&mut Self>) {
        let reachability = ffi::reachability();
        // Qt: Unknown=0, Disconnected=1, Local=2, Site=3, Online=4.
        let connected = if reachability != 0 {
            reachability != 1
        } else {
            ffi::interface_flags()
                .iter()
                .any(|f| f & 1 != 0 && f & 2 != 0 && f & 8 == 0)
        };
        self.send(Command::Network(connected, matches!(reachability, 2 | 3)));
    }
    fn update_pacman_view(mut self: Pin<&mut Self>, enabled: bool) {
        if self.rust().pacman_view != enabled {
            self.as_mut().rust_mut().pacman_view = enabled;
            self.pacman_view_changed();
        }
    }
    fn set_pacman_view(mut self: Pin<&mut Self>, enabled: bool) {
        if self.rust().pacman_view == enabled {
            return;
        }
        if desktop::save_settings(
            &self.rust().settings,
            "Interface",
            &[("PacmanView", enabled.to_string())],
        ) {
            self.as_mut().update_pacman_view(enabled);
        }
    }
    fn save_update_window_state(
        mut self: Pin<&mut Self>,
        width: i32,
        height: i32,
        maximized: bool,
    ) {
        let width = width.max(480);
        let height = height.max(400);
        if desktop::save_settings(
            &self.rust().settings,
            "UpdateWindow",
            &[
                ("Width", width.to_string()),
                ("Height", height.to_string()),
                ("Maximized", maximized.to_string()),
            ],
        ) {
            self.as_mut().set_update_window_width(width);
            self.as_mut().set_update_window_height(height);
            self.set_update_window_maximized(maximized);
        }
    }
    fn copy_signing_key_technical_details(self: Pin<&mut Self>) {
        if self.rust().signing_key_security_error {
            ffi::copy_text(&self.rust().signing_key_technical_details);
        }
    }
    fn open_signing_key_issue(self: Pin<&mut Self>) {
        if self.rust().signing_key_security_error {
            let url = self.rust().signing_key_issue_url.to_string();
            if desktop::issue_url_allowed(&url) {
                ffi::open_url(&url);
            }
        }
    }
    fn host_title(&self) -> QString {
        QString::from(&format!(
            "{} — {}",
            ffi::translate("System Updates", "", -1, &vec![]),
            ffi::translate("Fluff Linux Update", "", -1, &vec![])
        ))
    }
}
