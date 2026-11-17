//! Reboot requirements belong to one boot, not to a window or login session.
use crate::runtime::{read_json, write_json};
use serde_json::{Value, json};
use std::{collections::BTreeMap, fs, process::Command};
pub const PATH: &str = "/etc/pacman.d/flufflinux-update-reboot.json";
pub const BANNER: &str =
    "A restart is strongly recommended because important system components were updated.";
pub const BLOCKED: &str = "Restart your computer before installing more updates to complete the previous update and help keep your system stable.";
pub const TITLE: &str = "Restart recommended";
pub const BODY: &str = "Important system components were updated. Until you restart, some applications or devices may not work correctly. Save your work before restarting.";
pub const ACTION: &str = "Restart";
pub const MESSAGES: &[&str] = &[BANNER, BLOCKED, TITLE, BODY, ACTION];
fn output(program: &str, args: &[&str]) -> String {
    Command::new(program)
        .args(args)
        .env("LC_ALL", "C")
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .unwrap_or_default()
}
fn boot() -> String {
    fs::read_to_string("/proc/sys/kernel/random/boot_id")
        .unwrap_or_default()
        .trim()
        .to_owned()
}
pub fn current() -> Value {
    let value = read_json(PATH);
    if valid_for_boot(&value, &boot()) {
        value
    } else {
        json!({})
    }
}
pub fn valid_for_boot(value: &Value, boot: &str) -> bool {
    !boot.is_empty() && value["boot_id"] == boot
}
pub fn blocked() -> bool {
    current()["blocked"] == true
}
fn versions() -> Option<BTreeMap<String, String>> {
    let result = Command::new("/usr/bin/pacman")
        .arg("-Q")
        .env("LC_ALL", "C")
        .output()
        .ok()?;
    if !result.status.success() {
        return None;
    }
    Some(
        String::from_utf8_lossy(&result.stdout)
            .lines()
            .filter_map(|line| line.split_once(' '))
            .map(|(name, version)| (name.to_owned(), version.to_owned()))
            .collect(),
    )
}
fn add_module_owners(
    watched: &mut BTreeMap<String, bool>,
    versions: &BTreeMap<String, String>,
    owners: &str,
    critical: bool,
) {
    for owner in owners.lines().map(str::trim) {
        if versions.contains_key(owner) {
            watched
                .entry(owner.to_owned())
                .and_modify(|value| *value |= critical)
                .or_insert(critical);
        }
    }
}
pub struct Snapshot {
    boot: String,
    versions: BTreeMap<String, String>,
    watched: BTreeMap<String, bool>,
}
impl Snapshot {
    pub fn capture() -> Self {
        let versions = versions().unwrap_or_default();
        let release = output("/usr/bin/uname", &["-r"]);
        let mut kernel = fs::read_to_string(format!("/usr/lib/modules/{release}/pkgbase"))
            .unwrap_or_default()
            .trim()
            .to_owned();
        if kernel.is_empty() {
            kernel = output(
                "/usr/bin/pacman",
                &["-Qqo", &format!("/usr/lib/modules/{release}/vmlinuz")],
            );
        }
        let mut watched = BTreeMap::new();
        if !kernel.is_empty() {
            watched.insert(kernel.clone(), true);
        }
        let loaded = fs::read_to_string("/proc/modules").unwrap_or_default();
        let modules: Vec<&str> = loaded
            .lines()
            .filter_map(|l| l.split_whitespace().next())
            .collect();
        let mut module_files = BTreeMap::new();
        for module in &modules {
            let file = output("/usr/bin/modinfo", &["-n", module]);
            if file.starts_with('/') {
                let critical = *module == "nvidia" || *module == "zfs";
                module_files
                    .entry(file)
                    .and_modify(|value| *value |= critical)
                    .or_insert(critical);
            }
        }
        // Pacman reloads its installed-file database for each invocation.
        // Query paths together, keeping critical owners in their own group.
        for critical in [false, true] {
            let paths: Vec<_> = module_files
                .iter()
                .filter(|(_, required)| **required == critical)
                .map(|(path, _)| path.as_str())
                .collect();
            // Bound argv size even on hosts with unusually many modules.
            for chunk in paths.chunks(128) {
                if let Ok(result) = Command::new("/usr/bin/pacman")
                    .args(["-Qqo", "--"])
                    .args(chunk)
                    .env("LC_ALL", "C")
                    .output()
                {
                    // An unowned DKMS path makes Pacman exit nonzero, while
                    // stdout still lists valid owners of other paths.
                    add_module_owners(
                        &mut watched,
                        &versions,
                        &String::from_utf8_lossy(&result.stdout),
                        critical,
                    );
                }
            }
        }
        // Module files generated by DKMS have no Pacman owner. Match the
        // installed driver families only when their module is actually loaded.
        for name in versions.keys() {
            if (modules.contains(&"nvidia")
                && (name == "nvidia-utils"
                    || name.starts_with("nvidia-")
                        && (name.ends_with("-dkms") || name.ends_with("-utils"))))
                || (modules.contains(&"zfs") && (name == "zfs-dkms" || name == "zfs-utils"))
            {
                watched.insert(name.clone(), true);
            }
        }
        for (module, package) in [
            ("wl", "broadcom-wl-dkms"),
            ("vboxdrv", "virtualbox-host-dkms"),
            ("v4l2loopback", "v4l2loopback-dkms"),
        ] {
            if modules.contains(&module) && versions.contains_key(package) {
                watched.insert(package.into(), false);
            }
        }
        // Never let a generic module-owner entry downgrade the running kernel.
        if !kernel.is_empty() {
            watched.insert(kernel, true);
        }
        let processes = fs::read_dir("/proc")
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|e| fs::read_to_string(e.path().join("comm")).ok())
            .collect::<Vec<_>>();
        if processes.iter().any(|p| p.trim() == "plasmashell") {
            watched.insert("plasma-workspace".into(), false);
        }
        if processes.iter().any(|p| p.trim().starts_with("kwin")) {
            watched.insert("kwin".into(), false);
        }
        Self {
            boot: boot(),
            versions,
            watched,
        }
    }
    fn requirement(&self, after: &BTreeMap<String, String>, previous: &Value) -> Option<Value> {
        let changed: Vec<_> = self
            .watched
            .iter()
            .filter(|(p, _)| self.versions.get(*p) != after.get(*p))
            .collect();
        if changed.is_empty() || self.boot.is_empty() {
            return None;
        }
        let blocked = previous["blocked"] == true || changed.iter().any(|(_, block)| **block);
        Some(
            json!({"boot_id":self.boot,"blocked":blocked,"recommended":true,
            "packages":changed.iter().map(|(p,_)| p.as_str()).collect::<Vec<_>>()}),
        )
    }
    pub fn record(&self) -> bool {
        let Some(after) = versions() else {
            return false;
        };
        if self.boot != boot() {
            return true;
        }
        self.requirement(&after, &current())
            .is_none_or(|value| write_json(PATH, &value))
    }
}
// The snapshot is root-owned and replaced for every transaction. Parent identity
// prevents an aborted transaction's snapshot from being reused by another one.
const BEFORE: &str = "/etc/pacman.d/flufflinux-update-reboot-before.json";
fn transaction() -> Option<String> {
    let pid = unsafe { libc::getppid() };
    let stat = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let start = stat.rsplit_once(") ")?.1.split_whitespace().nth(19)?;
    Some(format!("{pid}:{start}"))
}
fn live_root() -> bool {
    use std::os::unix::fs::MetadataExt;
    match (fs::metadata("/"), fs::metadata("/proc/1/root")) {
        (Ok(root), Ok(init)) => root.dev() == init.dev() && root.ino() == init.ino(),
        _ => false,
    }
}
/// Observational Pacman hooks. These never acquire an inhibitor or gate Pacman.
/// Chroots and offline installation targets must not inherit the live boot.
pub fn pacman_hook(before: bool) -> bool {
    if unsafe { libc::geteuid() } != 0 || !live_root() {
        return true;
    }
    let Some(transaction) = transaction() else {
        return false;
    };
    if before {
        let _ = fs::remove_file(BEFORE);
        let snapshot = Snapshot::capture();
        if snapshot.boot.is_empty() || snapshot.versions.is_empty() {
            return true;
        }
        write_json(
            BEFORE,
            &json!({"boot_id":snapshot.boot, "transaction":transaction,
            "versions":snapshot.versions, "watched":snapshot.watched}),
        )
    } else {
        let value = read_json(BEFORE);
        let _ = fs::remove_file(BEFORE);
        if value["transaction"] != transaction || !valid_for_boot(&value, &boot()) {
            return true;
        }
        let (Ok(versions), Ok(watched)) = (
            serde_json::from_value(value["versions"].clone()),
            serde_json::from_value(value["watched"].clone()),
        ) else {
            return false;
        };
        Snapshot {
            boot: boot(),
            versions,
            watched,
        }
        .record()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_changed_active_components_trigger_and_blocks_never_downgrade() {
        let before = BTreeMap::from([
            ("linux".into(), "1".into()),
            ("linux-lts".into(), "1".into()),
            ("kwin".into(), "1".into()),
            ("zfs-dkms".into(), "1".into()),
        ]);
        let snapshot = Snapshot {
            boot: "one".into(),
            versions: before.clone(),
            watched: BTreeMap::from([
                ("linux".into(), true),
                ("kwin".into(), false),
                ("zfs-dkms".into(), true),
            ]),
        };
        let mut after = before.clone();
        after.insert("linux-lts".into(), "2".into());
        assert!(snapshot.requirement(&after, &json!({})).is_none());
        after.insert("kwin".into(), "2".into());
        assert_eq!(
            snapshot.requirement(&after, &json!({})).unwrap()["blocked"],
            false
        );
        assert_eq!(
            snapshot
                .requirement(&after, &json!({"blocked":true}))
                .unwrap()["blocked"],
            true
        );
        for name in ["linux", "zfs-dkms"] {
            let mut after = before.clone();
            after.insert(name.into(), "2".into());
            assert_eq!(
                snapshot.requirement(&after, &json!({})).unwrap()["blocked"],
                true
            );
        }
    }
    #[test]
    fn batched_owners_deduplicate_and_never_downgrade_critical_packages() {
        let versions = BTreeMap::from([
            ("linux".into(), "1".into()),
            ("nvidia".into(), "1".into()),
            ("external-driver".into(), "1".into()),
        ]);
        let mut watched = BTreeMap::from([("linux".into(), true)]);
        add_module_owners(
            &mut watched,
            &versions,
            "linux\nexternal-driver\nexternal-driver\nunknown\n",
            false,
        );
        add_module_owners(&mut watched, &versions, "nvidia\nexternal-driver\n", true);
        add_module_owners(&mut watched, &versions, "nvidia\n", false);
        assert_eq!(
            watched,
            BTreeMap::from([
                ("linux".into(), true),
                ("nvidia".into(), true),
                ("external-driver".into(), true),
            ])
        );
    }
    #[test]
    fn requirements_expire_only_on_new_boot() {
        let value = json!({"boot_id":"one","blocked":true});
        assert!(valid_for_boot(&value, "one"));
        assert!(!valid_for_boot(&value, "two"));
        assert!(!valid_for_boot(&json!({"boot_id":""}), ""));
    }
}
