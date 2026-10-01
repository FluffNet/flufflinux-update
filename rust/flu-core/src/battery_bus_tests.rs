//! Actual D-Bus wire tests on a private bus; never contacts/reconfigures UPower
//! or inserts devices into the user's system. Requires dbus-daemon on Linux.
use super::*;
use std::{
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::{Arc, Mutex},
};

struct PrivateBus(Child);
impl Drop for PrivateBus {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

#[derive(Clone, Copy)]
struct Reading {
    kind: u32,
    present: bool,
    state: u32,
    percentage: f64,
}
struct Device(Arc<Mutex<Reading>>);

#[zbus::interface(name = "org.freedesktop.UPower.Device")]
impl Device {
    #[zbus(property, name = "Type")]
    fn kind(&self) -> u32 {
        self.0.lock().unwrap().kind
    }
    #[zbus(property)]
    fn is_present(&self) -> bool {
        self.0.lock().unwrap().present
    }
    #[zbus(property)]
    fn state(&self) -> u32 {
        self.0.lock().unwrap().state
    }
    #[zbus(property)]
    fn percentage(&self) -> f64 {
        self.0.lock().unwrap().percentage
    }
    // Deliberately false even on the display object. Real UPower does not
    // guarantee this property for the composite, only for physical devices.
    #[zbus(property)]
    fn power_supply(&self) -> bool {
        false
    }
}

#[test]
fn dbus_system_power_changes_exclude_low_accessories_and_recover_after_service_loss() {
    let mut bus = PrivateBus(
        Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--print-address=1"])
            .stdout(Stdio::piped())
            .spawn()
            .expect("dbus-daemon is required for the isolated UPower wire test"),
    );
    let mut address = String::new();
    BufReader::new(bus.0.stdout.take().unwrap())
        .read_line(&mut address)
        .unwrap();
    let address = address.trim();
    let value = Arc::new(Mutex::new(Reading {
        kind: 0,
        present: false,
        state: 0,
        percentage: 0.0,
    }));
    // Reproduce a mouse exposed as the generic Battery kind at 19%.
    let mouse = Arc::new(Mutex::new(Reading {
        kind: 2,
        present: true,
        state: 2,
        percentage: 19.0,
    }));
    let service = Builder::address(address)
        .unwrap()
        .name(SERVICE)
        .unwrap()
        .serve_at(DISPLAY_DEVICE, Device(value.clone()))
        .unwrap()
        .serve_at(
            "/org/freedesktop/UPower/devices/battery_mouse",
            Device(mouse),
        )
        .unwrap()
        .build()
        .unwrap();
    let client = Builder::address(address)
        .unwrap()
        .method_timeout(CALL_TIMEOUT)
        .build()
        .unwrap();
    let mut monitor = BatteryMonitor {
        connection: Some(client.clone()),
        ..Default::default()
    };
    for (description, kind, present, state, percentage, expected) in [
        ("desktop with only a 19% mouse", 0, false, 0, 0.0, false),
        ("healthy laptop plus 19% mouse", 2, true, 2, 80.0, false),
        ("low system battery plus mouse", 2, true, 2, 19.0, true),
        (
            "battery exactly at warning threshold",
            2,
            true,
            2,
            20.0,
            true,
        ),
        ("AC reconnected and charging", 2, true, 1, 19.0, false),
        ("low UPS on desktop or laptop", 3, true, 2, 19.0, true),
        ("UPS back on mains", 3, true, 1, 19.0, false),
        (
            "healthy composite of multiple batteries",
            2,
            true,
            2,
            60.0,
            false,
        ),
        (
            "low composite of multiple batteries",
            2,
            true,
            2,
            10.0,
            true,
        ),
        ("removed battery", 2, false, 2, 10.0, false),
        ("empty system supply", 2, true, 3, 0.0, true),
    ] {
        *value.lock().unwrap() = Reading {
            kind,
            present,
            state,
            percentage,
        };
        monitor.last_poll = None;
        assert_eq!(monitor.is_low(true), expected, "{description}");
    }
    service.release_name(SERVICE).unwrap();
    monitor.last_poll = None;
    let start = Instant::now();
    assert!(!monitor.is_low(true));
    assert!(monitor.connection.is_none());
    assert!(start.elapsed() < Duration::from_secs(2));

    service.request_name(SERVICE).unwrap();
    // Reconnect to the same PRIVATE bus, not the machine's system bus.
    monitor.connection = Some(client);
    monitor.last_poll = None;
    assert!(monitor.is_low(true));
    assert!(!monitor.is_low(false));
    assert!(monitor.is_low(true));
}
