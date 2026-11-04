//! Live-system entry point for an installer-owned target update.
mod power;

use serde_json::json;
use std::{
    fs,
    io::Write,
    os::unix::fs::MetadataExt,
    path::{Path, PathBuf},
    process::{Command, ExitCode, Stdio},
};

fn target_root(value: &str) -> Result<PathBuf, String> {
    let root = fs::canonicalize(value).map_err(|e| e.to_string())?;
    let target = fs::metadata(&root).map_err(|e| e.to_string())?;
    let host = fs::metadata("/").map_err(|e| e.to_string())?;
    if !Path::new(value).is_absolute()
        || root == Path::new("/")
        || (target.dev() == host.dev() && target.ino() == host.ino())
    {
        return Err("The running system cannot be an installer target".into());
    }
    if !Command::new("/usr/bin/mountpoint")
        .arg("-q")
        .arg(&root)
        .status()
        .is_ok_and(|s| s.success())
    {
        return Err("The installer target must be mounted".into());
    }
    if !fs::read_to_string(root.join("usr/share/flufflinux-update/installer-protocol-version"))
        .is_ok_and(|v| v.trim() == "1")
    {
        return Err("The target requires FLU installer protocol 1 (FLU 1.7 or newer)".into());
    }
    for required in [
        "etc/pacman.conf",
        "usr/bin/pacman",
        "usr/lib/flufflinux-update/flufflinux-update-worker",
    ] {
        if !root.join(required).is_file() {
            return Err(format!("Missing target file: {required}"));
        }
    }
    Ok(root)
}

fn execute() -> Result<bool, String> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    let [mode, flag, path] = args.as_slice() else {
        return Err("Usage: flufflinux-update-installer --installer --target-root /mnt".into());
    };
    if mode != "--installer" || flag != "--target-root" || unsafe { libc::geteuid() } != 0 {
        return Err("Installer mode requires root and an explicit target root".into());
    }
    let root = target_root(path)?;
    // The live system owns inhibition. The target has no running logind or
    // systemd service manager. Keep the descriptor until arch-chroot exits.
    let _inhibitor =
        power::SleepInhibitor::acquire().map_err(|e| format!("Sleep inhibition failed: {e}"))?;
    let status = Command::new("/usr/bin/arch-chroot")
        .arg(root)
        .args([
            "/usr/lib/flufflinux-update/flufflinux-update-worker",
            "--installer",
        ])
        .env("LC_ALL", "C")
        .stdin(Stdio::null())
        .status()
        .map_err(|e| e.to_string())?;
    Ok(status.success())
}

fn main() -> ExitCode {
    match execute() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(error) => {
            eprintln!("{error}");
            let _ = writeln!(
                std::io::stdout(),
                "{}",
                json!({"protocol":1,"event":"finished","code":1,"state":{"phase":"failed","error":"INSTALLER_START_FAILED"},"detail":error})
            );
            ExitCode::FAILURE
        }
    }
}
