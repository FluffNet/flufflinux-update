//! Native logind sleep inhibition, owned by the background worker, not the UI.
use std::time::Duration;
use zbus::{blocking::connection::Builder, zvariant::OwnedFd};

pub struct SleepInhibitor {
    // logind releases the lock when this descriptor closes, including if the
    // worker is cancelled or killed. No separate unlock request is necessary.
    _lock: OwnedFd,
}

impl SleepInhibitor {
    pub fn acquire() -> zbus::Result<Self> {
        let connection = Builder::system()?
            .method_timeout(Duration::from_secs(5))
            .build()?;
        let reply = connection.call_method(
            Some("org.freedesktop.login1"),
            "/org/freedesktop/login1",
            Some("org.freedesktop.login1.Manager"),
            "Inhibit",
            &(
                "sleep",
                "Fluff Linux Update",
                "Downloading and installing system updates",
                "block",
            ),
        )?;
        // The descriptor, not the D-Bus connection, owns the inhibition. Sleep
        // covers suspend and hibernation; do not inhibit screen blanking/locking
        // (idle), shutdown, or hardware-key handling as a side effect.
        Ok(Self {
            _lock: reply.body().deserialize()?,
        })
    }
}
