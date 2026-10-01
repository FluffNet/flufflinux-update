//! Isolated integration driver; Cargo example, never installed or linked into FLU.
use flu_core::desktop::{Command, Controller};
use std::{
    fs,
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};
fn main() {
    let host = std::env::var("FLU_HOST_MOUNT_NS").expect("namespace fixture required");
    assert_ne!(
        fs::read_link("/proc/self/ns/mnt")
            .unwrap()
            .to_string_lossy(),
        host
    );
    let translator = Arc::new(|message: &str, plural: &str, count: i64, args: &[String]| {
        let mut result = if count >= 0 && count != 1 {
            plural.to_owned()
        } else {
            message.to_owned()
        };
        if count >= 0 {
            result = result.replace("%1", &count.to_string());
        }
        for (index, value) in args.iter().enumerate() {
            result = result.replace(&format!("%{}", index + 1), value);
        }
        result
    });
    let mut controller = Controller::new(translator, |_| {}, Arc::new(AtomicBool::new(false)));
    controller.read_last();
    controller.read_install();
    match std::env::args().nth(1).as_deref() {
        Some("check") => controller.command(Command::Check),
        Some("install") => {
            controller.command(Command::Check);
            assert_eq!(controller.model["checkError"], "");
            assert_eq!(controller.model["updatesAvailable"], true);
            controller.command(Command::Install);
            let start = Instant::now();
            while matches!(
                controller.model["installPhase"].as_str(),
                Some("starting" | "downloading" | "installing")
            ) {
                assert!(
                    start.elapsed() < Duration::from_secs(60),
                    "worker timed out"
                );
                std::thread::sleep(Duration::from_millis(50));
                controller.read_install();
            }
        }
        Some("reconnect") => {}
        Some("cancel") => controller.command(Command::Cancel),
        _ => panic!("expected check/install/reconnect/cancel"),
    }
    println!("{}", controller.model);
}
