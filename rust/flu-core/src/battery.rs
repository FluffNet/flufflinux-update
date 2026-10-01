//! System power only: UPower's composite excludes peripheral batteries and
//! supports UPSes as well as single/multiple system batteries on any chassis.
//! Contract: https://upower.freedesktop.org/docs/UPower.html#UPower.GetDisplayDevice
use std::{
    collections::HashMap,
    time::{Duration, Instant},
};
use zbus::{
    blocking::{Connection, connection::Builder},
    zvariant::OwnedValue,
};

const SERVICE: &str = "org.freedesktop.UPower";
const DISPLAY_DEVICE: &str = "/org/freedesktop/UPower/devices/DisplayDevice";
const DEVICE_INTERFACE: &str = "org.freedesktop.UPower.Device";
const POLL_INTERVAL: Duration = Duration::from_secs(5);
const CALL_TIMEOUT: Duration = Duration::from_millis(750);
type Properties = HashMap<String, OwnedValue>;

#[derive(Default)]
pub(crate) struct BatteryMonitor {
    connection: Option<Connection>,
    last_poll: Option<Instant>,
    low: bool,
}

impl BatteryMonitor {
    // Called on the existing controller thread, never Qt's GUI thread. Reuse
    // the bus connection and bound calls so a power-service failure cannot
    // indefinitely hold up controller commands.
    pub(crate) fn is_low(&mut self, relevant: bool) -> bool {
        if !relevant {
            self.last_poll = None;
            self.low = false;
            return false;
        }
        if self.last_poll.is_some_and(|t| t.elapsed() < POLL_INTERVAL) {
            return self.low;
        }
        self.last_poll = Some(Instant::now());
        match self.read() {
            Ok(low) => self.low = low,
            Err(_) => {
                // Unknown is not a measured low battery. Retry on the next
                // poll; never fall back to treating arbitrary sysfs Battery
                // devices (including mice) as system power supplies.
                self.connection = None;
                self.low = false;
            }
        }
        self.low
    }

    fn read(&mut self) -> zbus::Result<bool> {
        if self.connection.is_none() {
            self.connection = Some(Builder::system()?.method_timeout(CALL_TIMEOUT).build()?);
        }
        read_display(self.connection.as_ref().unwrap())
    }
}

fn read_display(connection: &Connection) -> zbus::Result<bool> {
    // UPower guarantees this path. Read one coherent snapshot, rather than
    // enumerating all batteries or inferring their role from names or a lid.
    let reply = connection.call_method(
        Some(SERVICE),
        DISPLAY_DEVICE,
        Some("org.freedesktop.DBus.Properties"),
        "GetAll",
        &(DEVICE_INTERFACE,),
    )?;
    display_is_low(&reply.body().deserialize()?)
}

fn property<'a>(properties: &'a Properties, name: &str) -> zbus::Result<&'a OwnedValue> {
    properties
        .get(name)
        .ok_or_else(|| zbus::Error::Failure(format!("UPower omitted {name}")))
}

fn display_is_low(properties: &Properties) -> zbus::Result<bool> {
    if !bool::try_from(property(properties, "IsPresent")?)? {
        return Ok(false);
    }
    // Type 2 = Battery; Type 3 = UPS. DisplayDevice is already restricted by
    // UPower to system supplies. Its PowerSupply property is NOT part of the
    // documented composite contract, so requiring it could hide real batteries.
    let kind = u32::try_from(property(properties, "Type")?)?;
    if !matches!(kind, 2 | 3) {
        return Ok(false);
    }
    // Preserve the <=20% warning for a discharging supply; an empty supply
    // also needs the warning. Charging/charged/unknown states do not.
    let state = u32::try_from(property(properties, "State")?)?;
    let percentage = f64::try_from(property(properties, "Percentage")?)?;
    Ok(matches!(state, 2 | 3) && (0.0..=20.0).contains(&percentage))
}

#[cfg(all(test, target_os = "linux"))]
#[path = "battery_bus_tests.rs"]
mod bus_tests;

#[cfg(test)]
mod tests {
    use super::*;

    fn display(kind: u32, present: bool, state: u32, percentage: f64) -> Properties {
        HashMap::from([
            ("Type".into(), kind.into()),
            ("IsPresent".into(), present.into()),
            ("State".into(), state.into()),
            ("Percentage".into(), percentage.into()),
        ])
    }

    #[test]
    fn low_system_batteries_and_ups_warn_without_chassis_or_name_filters() {
        for kind in [2, 3] {
            for percentage in [0.0, 1.0, 19.0, 20.0] {
                // Composite PowerSupply, NativePath, model and lid are
                // deliberately absent: none is required to recognize it.
                assert!(display_is_low(&display(kind, true, 2, percentage)).unwrap());
            }
        }
    }

    #[test]
    fn peripheral_only_desktop_and_removed_battery_do_not_warn() {
        // With only accessories UPower exposes no present display battery.
        assert!(!display_is_low(&display(0, false, 0, 0.0)).unwrap());
        assert!(!display_is_low(&display(2, false, 2, 19.0)).unwrap());
        // Defensively reject every other published UPower kind, even if a
        // broken service supplies one at the composite path.
        for kind in (0..=28).filter(|k| !matches!(k, 2 | 3)) {
            assert!(!display_is_low(&display(kind, true, 2, 19.0)).unwrap());
        }
    }

    #[test]
    fn charging_full_and_unknown_states_do_not_warn_but_empty_does() {
        for kind in [2, 3] {
            for state in [0, 1, 4, 5, 6] {
                assert!(!display_is_low(&display(kind, true, state, 19.0)).unwrap());
            }
            assert!(display_is_low(&display(kind, true, 3, 0.0)).unwrap());
        }
    }

    #[test]
    fn healthy_and_invalid_percentages_are_not_low() {
        for percentage in [20.01, 50.0, 100.0, -1.0, 101.0, f64::NAN, f64::INFINITY] {
            assert!(!display_is_low(&display(2, true, 2, percentage)).unwrap());
        }
    }

    #[test]
    fn missing_or_malformed_readings_are_not_fabricated_as_zero() {
        for name in ["IsPresent", "Type", "State", "Percentage"] {
            let mut properties = display(2, true, 2, 19.0);
            properties.remove(name);
            assert!(display_is_low(&properties).is_err());
            properties.insert(name.into(), 0i32.into());
            assert!(display_is_low(&properties).is_err());
        }
    }

    #[test]
    fn irrelevant_update_state_clears_cached_warning_without_querying_bus() {
        let mut monitor = BatteryMonitor {
            low: true,
            last_poll: Some(Instant::now()),
            ..Default::default()
        };
        assert!(monitor.is_low(true));
        assert!(!monitor.is_low(false));
        assert!(monitor.last_poll.is_none());
        assert!(monitor.connection.is_none());
    }
}
