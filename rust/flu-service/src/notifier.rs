//! Unprivileged, D-Bus-activated notification observer; no package operations.
fn main() {
    if unsafe { libc::geteuid() } == 0 {
        eprintln!("FLU notifications must run in the regular user's session.");
        std::process::exit(1);
    }
    if let Err(error) = flu_core::notifications::run() {
        eprintln!("FLU notifications: {error}");
        std::process::exit(1);
    }
}
