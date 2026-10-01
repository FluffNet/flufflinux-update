#!/usr/bin/env python3
"""Exercise production FLU binaries with real Pacman in a disposable namespace.

Run with sudo on a disposable Linux development machine. No test switches or
mock commands are added to the shipped binaries. Service and authorization
launchers are substituted only inside the private namespace, to run the real
compiled helper and worker against the disposable package database.
The test repository deliberately contains unsigned, synthetic packages; signing
and trust behavior is covered separately by signingkeyrecoverytest.
"""

import argparse
import base64
import functools
import http.server
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import traceback


def run(args, *, expected=0, **kwargs):
    result = subprocess.run(
        [str(arg) for arg in args], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=120, **kwargs
    )
    if expected is not None and result.returncode != expected:
        raise AssertionError(f"{args}: exit {result.returncode}\n{result.stdout}")
    return result


def package(directory, name, version, *, metadata=(), payload=None, files=None):
    archive = directory / f"{name}-{version}-any.pkg.tar.gz"
    contents = files or {f"usr/share/flu-test/{name}": payload or version.encode()}
    info = "\n".join([
        f"pkgname = {name}", f"pkgver = {version}", "pkgdesc = FLU isolated test",
        "builddate = 1", "packager = FLU regression fixture", "arch = any",
        f"size = {sum(len(value) for value in contents.values())}", "license = MIT",
        *metadata, "",
    ]).encode()
    with tarfile.open(archive, "w:gz") as tar:
        for filename, data in {".PKGINFO": info, **contents}.items():
            entry = tarfile.TarInfo(filename)
            entry.size = len(data)
            entry.mode = 0o644
            tar.addfile(entry, io.BytesIO(data))
    return archive


class Server(http.server.SimpleHTTPRequestHandler):
    delay = 0.0
    unavailable = set()

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path.rsplit("/", 1)[-1] in self.unavailable:
            self.send_error(503, "Deliberate regression-test failure")
        elif self.headers.get("Range", "").startswith("bytes="):
            # Pacman resumes its .part file with a normal HTTP range request.
            path = Path(self.translate_path(self.path))
            start = int(self.headers["Range"].split("=", 1)[1].split("-", 1)[0])
            size = path.stat().st_size
            if start >= size:
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size - start))
            self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
            self.end_headers()
            with path.open("rb") as source:
                source.seek(start)
                self.copyfile(source, self.wfile)
        else:
            super().do_GET()

    def copyfile(self, source, destination):
        try:
            while data := source.read(64 * 1024):
                destination.write(data)
                destination.flush()
                if self.path.endswith(".pkg.tar.gz"):
                    time.sleep(self.delay)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Workflows:
    def __init__(self, base, helper, worker, controller=None):
        self.base, self.helper, self.worker = base, helper, worker
        self.controller = controller
        for name in ("root", "db", "cache", "etc", "tmp", "repo", "hooks"):
            (base / name).mkdir()
        (base / "pacman.log").touch()
        handler = functools.partial(Server, directory=str(base / "repo"))
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        port = self.server.server_address[1]
        (base / "pacman.conf").write_text(
            f"[options]\nRootDir = {base}/root\nDBPath = /var/lib/pacman\n"
            f"CacheDir = /var/cache/pacman/pkg\nLogFile = /var/log/pacman.log\n"
            f"HookDir = {base}/hooks\nArchitecture = auto\nSigLevel = Never\n"
            f"[flu-test]\nServer = http://127.0.0.1:{port}\n"
        )
        # The parent first created a private mount and PID namespace. Refuse to
        # bind anything if that isolation was lost; these paths would be live.
        assert os.getpid() == 1, "workflow runner must be namespace PID 1"
        assert os.readlink("/proc/self/ns/mnt") != os.environ["FLU_HOST_MOUNT_NS"]
        for source, target in (
            (base / "db", "/var/lib/pacman"),
            (base / "cache", "/var/cache/pacman/pkg"),
            (base / "etc", "/etc/pacman.d"),
            (base / "tmp", "/tmp"),
            (base / "pacman.log", "/var/log/pacman.log"),
            (base / "pacman.conf", "/etc/pacman.conf"),
        ):
            run(["mount", "--bind", source, target])
        self.checkdb = Path("/tmp/flufflinux-checkupdates-1000")
        self.checkdb.mkdir()
        self.state = base / "etc/flufflinux-update-state.json"
        self.policy = base / "etc/flufflinux-update-package-protection.json"
        self.policy.write_text(json.dumps({"protected": [], "warning": []}))
        stub = base / "systemctl"
        stub.write_text(
            "#!/usr/bin/python3\n"
            "import os, signal, subprocess, sys, time\n"
            f"worker = {str(worker)!r}\nbase = {str(base)!r}\n"
            "assert len(sys.argv) == 3 and sys.argv[2] == 'flufflinux-update.service'\n"
            "pidfile = base + '/worker.pid'\n"
            "if sys.argv[1] == 'start':\n"
            "    with open(base + '/worker.stdout', 'ab') as output:\n"
            "        child = subprocess.Popen([worker], stdout=output, stderr=output, start_new_session=True)\n"
            "    with open(pidfile, 'w') as file: file.write(str(child.pid))\n"
            "elif sys.argv[1] == 'stop':\n"
            "    pid = int(open(pidfile).read())\n"
            "    cmd = '/proc/' + str(pid) + '/cmdline'\n"
            "    if os.path.exists(cmd):\n"
            "        assert open(cmd, 'rb').read().split(b'\\0')[0] == worker.encode()\n"
            "        children = '/proc/' + str(pid) + '/task/' + str(pid) + '/children'\n"
            "        for child in open(children).read().split():\n"
            "            try: os.kill(int(child), signal.SIGTERM)\n"
            "            except ProcessLookupError: pass\n"
            "        time.sleep(0.3)\n"
            "        try: os.killpg(pid, signal.SIGTERM)\n"
            "        except ProcessLookupError: pass\n"
            "else: sys.exit(2)\n"
        )
        stub.chmod(0o755)
        run(["mount", "--bind", stub, "/usr/bin/systemctl"])
        if controller:
            # Root is already authorized inside this private fixture; replace
            # only the polkit launcher here to exercise the real controller.
            launcher = base / "pkexec"
            launcher.write_text("#!/bin/sh\nexport PKEXEC_UID=0\nexec \"$@\"\n")
            launcher.chmod(0o755)
            run(["mount", "--bind", launcher, "/usr/bin/pkexec"])
            run(["mount", "--bind", helper, "/usr/lib/flufflinux-update/flufflinux-update-helper"])
        self.results = []

    def pacman(self, *args, expected=0):
        return run(["/usr/bin/pacman", *args], expected=expected,
                   env={**os.environ, "LC_ALL": "C"})

    def installed(self, name):
        result = self.pacman("-Q", name, expected=None)
        return result.stdout.strip() if result.returncode == 0 else None

    def refresh(self, *packages):
        repo = self.base / "repo"
        for old in repo.glob("flu-test.db*"):
            old.unlink()
        for old in repo.glob("flu-test.files*"):
            old.unlink()
        run(["repo-add", repo / "flu-test.db.tar.gz", *packages])
        self.pacman("-Syy", "--noconfirm")
        local = self.checkdb / "local"
        if not local.exists():
            local.symlink_to("/var/lib/pacman/local", target_is_directory=True)
        self.pacman("--dbpath", self.checkdb, "-Syy", "--noconfirm")

    def plan(self, expected=None):
        return run([self.helper, self.checkdb], expected=expected,
                   env={**os.environ, "PKEXEC_UID": "1000"})

    def state_value(self):
        try:
            return json.loads(self.state.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return {}

    def start(self):
        encoded = base64.urlsafe_b64encode(b"[]").decode()
        run([self.helper, "--install", "fixture", "fixture", "false", encoded])

    @staticmethod
    def sleep_inhibitors():
        result = run(["busctl", "--system", "--json=short", "call",
                      "org.freedesktop.login1", "/org/freedesktop/login1",
                      "org.freedesktop.login1.Manager", "ListInhibitors"])
        return [entry for entry in json.loads(result.stdout)["data"][0]
                if entry[1] == "Fluff Linux Update"]

    def assert_sleep_inhibited(self):
        entries = self.sleep_inhibitors()
        assert len(entries) == 1, entries
        what, who, why, mode, uid, pid = entries[0]
        assert what == "sleep" and mode == "block" and uid == 0 and pid > 1, entries
        assert why == "Downloading and installing system updates", entries

    def assert_sleep_released(self):
        until = time.monotonic() + 5
        while time.monotonic() < until:
            self.reap()
            if not self.sleep_inhibitors():
                return
            time.sleep(0.02)
        raise AssertionError(f"leaked sleep inhibitor: {self.sleep_inhibitors()}")

    def finish(self, *, timeout=30):
        snapshots = []
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            state = self.state_value()
            snapshots.append(state)
            if state.get("phase") in ("complete", "failed", "cancelled"):
                # Permit the worker to quit and reap adopted child processes.
                time.sleep(0.05)
                self.reap()
                self.assert_sleep_released()
                return state, snapshots
            time.sleep(0.01)
        raise AssertionError(f"worker timeout: {self.state_value()}")

    @staticmethod
    def reap():
        try:
            while os.waitpid(-1, os.WNOHANG)[0]:
                pass
        except ChildProcessError:
            pass

    def test(self, name, function):
        if getattr(self, "selected_test", None) and self.selected_test not in name:
            return
        started = time.monotonic()
        print(f"RUN {name}", flush=True)
        self.assert_sleep_released()
        details = function() or {}
        self.assert_sleep_released()
        result = {"test": name, "passed": True,
                  "seconds": round(time.monotonic() - started, 3), **details}
        self.results.append(result)
        print(f"PASS {name}: {json.dumps(details)}", flush=True)

    def replacement_and_update(self):
        repo = self.base / "repo"
        old = package(repo, "flu-test-old-calculator", "1-1")
        initial = package(repo, "flu-test-upgrade", "1-1")
        upgrade = package(repo, "flu-test-upgrade", "2-1", payload=os.urandom(8 * 1024 * 1024))
        replacement = package(repo, "flu-test-new-calculator", "1-1", metadata=[
            "replaces = flu-test-old-calculator",
        ])
        self.pacman("-U", "--noconfirm", old, initial)
        self.refresh(upgrade, replacement)
        plan = self.plan()
        assert "FLU_REPLACEMENT:flu-test-old-calculator|1-1|flu-test-new-calculator|1-1" in plan.stdout, plan.stdout
        assert "FLU_PLANNED_PACKAGE:flu-test-upgrade|2-1" in plan.stdout, plan.stdout
        assert self.installed("flu-test-old-calculator")
        assert self.installed("flu-test-upgrade") == "flu-test-upgrade 1-1"
        assert not self.installed("flu-test-new-calculator")
        Server.delay = 0.015
        self.start()
        state, snapshots = self.finish()
        Server.delay = 0
        assert state.get("phase") == "complete", state
        assert state["progress"] == 100, state
        assert self.installed("flu-test-upgrade") == "flu-test-upgrade 2-1"
        assert self.installed("flu-test-new-calculator")
        assert not self.installed("flu-test-old-calculator")
        assert any(s.get("phase") == "downloading" and 0 < s.get("progress", 0) < 100 for s in snapshots)
        assert any(s.get("phase") == "installing" for s in snapshots)
        assert list((self.base / "cache").glob("flu-test-upgrade-2-1*.pkg.tar.gz"))
        assert not (self.base / "db/db.lck").exists()
        return {"replacement": "replaces-only metadata", "phases": sorted({s.get("phase", "") for s in snapshots}),
                "download_progress_observed": True, "planner_did_not_install": True}

    def dependency_policy(self, kind):
        repo = self.base / "repo"
        library = package(repo, "flu-test-library", "1-1")
        newer = package(repo, "flu-test-library", "2-1")
        blocker = package(repo, "flu-test-blocker", "1-1", metadata=["depend = flu-test-library=1-1"])
        self.pacman("-U", "--noconfirm", library, blocker)
        self.refresh(newer)
        self.policy.write_text(json.dumps({"protected": ["flu-test-blocker"] if kind == "protected" else [],
                                          "warning": ["flu-test-blocker"] if kind == "warning" else []}))
        code = {"protected": 20, "warning": 21, "autoremove": 23}[kind]
        result = self.plan(expected=code)
        marker = {"protected": "FLU_PROTECTED_REMOVAL", "warning": "FLU_WARNING_DEPENDENCY",
                  "autoremove": "FLU_AUTOREMOVED"}[kind]
        assert f"{marker}:flu-test-blocker" in result.stdout, result.stdout
        assert bool(self.installed("flu-test-blocker")) == (kind != "autoremove")
        assert self.installed("flu-test-library") == "flu-test-library 1-1"
        assert not (self.checkdb / "db.lck").exists()
        if kind != "autoremove":
            run([self.helper, "--remove-package", "flu-test-blocker"])
        self.pacman("-R", "--noconfirm", "flu-test-library")
        return {"exit_code": code, "marker": marker}

    def file_conflict(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-conflict", "1-1")
        upgrade = package(repo, "flu-test-conflict", "2-1", files={
            "usr/share/flu-test/flu-test-conflict": b"2-1",
            "usr/share/flu-test/unmanaged": b"packaged replacement",
        })
        self.pacman("-U", "--noconfirm", initial)
        original = self.base / "root/usr/share/flu-test/unmanaged"
        original.write_bytes(b"preserve me")
        self.refresh(upgrade)
        self.start()
        state, _ = self.finish()
        assert state.get("phase") == "complete", state
        assert self.installed("flu-test-conflict") == "flu-test-conflict 2-1"
        preserved = Path(state["recovery_preserved_file"])
        assert preserved.is_relative_to(self.base / "root"), preserved
        assert preserved.read_bytes() == b"preserve me"
        assert original.read_bytes() == b"packaged replacement"
        return {"preserved_contents": True, "automatic_retry": True}

    def direct_conflict_policy(self, kind):
        repo = self.base / "repo"
        name = f"flu-test-direct-{kind}"
        blocker_name = f"{name}-blocker"
        initial = package(repo, name, "1-1")
        blocker = package(repo, blocker_name, "1-1")
        newer = package(repo, name, "2-1", metadata=[f"conflict = {blocker_name}"])
        self.pacman("-U", "--noconfirm", initial, blocker)
        self.refresh(newer)
        self.policy.write_text(json.dumps({"protected": [blocker_name] if kind == "protected" else [],
                                          "warning": [blocker_name] if kind == "warning" else []}))
        code = {"protected": 20, "warning": 21, "autoremove": 23}[kind]
        result = self.plan(expected=code)
        marker = {"protected": "FLU_PROTECTED_REMOVAL", "warning": "FLU_WARNING_REMOVAL",
                  "autoremove": "FLU_AUTOREMOVED"}[kind]
        assert f"{marker}:{blocker_name}" in result.stdout, result.stdout
        assert bool(self.installed(blocker_name)) == (kind != "autoremove")
        assert self.installed(name) == f"{name} 1-1"
        if kind != "autoremove":
            run([self.helper, "--remove-package", blocker_name])
        self.pacman("-R", "--noconfirm", name)
        return {"exit_code": code, "marker": marker, "planner_did_not_install": True}

    def mixed_dependency_policy(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-mixed", "1-1")
        newer = package(repo, "flu-test-mixed", "2-1")
        automatic = package(repo, "flu-test-a-automatic", "1-1", metadata=["depend = flu-test-mixed=1-1"])
        protected = package(repo, "flu-test-z-protected", "1-1", metadata=["depend = flu-test-mixed=1-1"])
        self.pacman("-U", "--noconfirm", initial, automatic, protected)
        self.refresh(newer)
        self.policy.write_text(json.dumps({"protected": ["flu-test-z-protected"], "warning": []}))
        result = self.plan(expected=20)
        assert "FLU_PROTECTED_REMOVAL:flu-test-z-protected" in result.stdout, result.stdout
        assert self.installed("flu-test-a-automatic")
        assert self.installed("flu-test-z-protected")
        self.pacman("-R", "--noconfirm", "flu-test-a-automatic", "flu-test-z-protected", "flu-test-mixed")
        return {"protected_precedes_automatic": True, "no_packages_removed": True}

    def owned_file_conflict(self):
        repo = self.base / "repo"
        owner = package(repo, "flu-test-owner", "1-1", files={"usr/share/flu-test/owned": b"keep owner"})
        initial = package(repo, "flu-test-owned-conflict", "1-1")
        newer = package(repo, "flu-test-owned-conflict", "2-1", files={"usr/share/flu-test/owned": b"conflicting payload"})
        self.pacman("-U", "--noconfirm", owner, initial)
        self.pacman("-Qo", self.base / "root/usr/share/flu-test/owned")
        self.refresh(newer)
        self.start()
        state, _ = self.finish()
        assert state["phase"] == "failed", state
        assert state["error"] == "INSTALL_FAILED", state
        original = self.base / "root/usr/share/flu-test/owned"
        assert original.read_bytes() == b"keep owner"
        assert not list(original.parent.glob("owned.preupdate*"))
        assert self.installed("flu-test-owned-conflict") == "flu-test-owned-conflict 1-1"
        return {"owned_file_unchanged": True, "not_renamed": True, "install_failed_safely": True}

    def record_current_update(self):
        run([self.helper, "--record-current-update"])
        value = json.loads((self.base / "etc/lastupdate.json").read_text())
        assert len(value["last_successful_system_update"]) >= 24, value
        return {"existing_lastupdate_contract": True}

    def desktop_controller(self):
        repo = self.base / "repo"
        first = package(repo, "flu-test-controller", "1-1")
        newer = package(repo, "flu-test-controller", "2-1")
        old = package(repo, "flu-test-controller-old", "1-1")
        replacement = package(repo, "flu-test-controller-new", "1-1", metadata=["replaces = flu-test-controller-old"])
        self.pacman("-U", "--noconfirm", first, old)
        self.policy.write_text(json.dumps({"protected": [], "warning": []}))
        self.refresh(newer, replacement)
        model = json.loads(run([self.controller, "check"]).stdout)
        assert model["checkComplete"] and model["updatesAvailable"], model
        assert model["checkError"] == "", model
        assert any(p["newName"] == "flu-test-controller-new" for p in model["updatePackages"]), model
        assert self.installed("flu-test-controller") == "flu-test-controller 1-1"
        model = json.loads(run([self.controller, "install"]).stdout)
        assert model["installPhase"] == "complete", model
        assert model["installationSuccessNotice"], model
        assert not model["updatesAvailable"], model
        assert self.installed("flu-test-controller") == "flu-test-controller 2-1"
        assert self.installed("flu-test-controller-new")
        assert not self.installed("flu-test-controller-old")
        self.reap()
        return {"real_checkupdates_and_planner": True, "replacement_visible": True,
                "controller_started_worker": True, "completion_presented": True}

    def cancellation(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-cancel", "1-1")
        newer = package(repo, "flu-test-cancel", "2-1", payload=os.urandom(8 * 1024 * 1024))
        self.pacman("-U", "--noconfirm", initial)
        self.refresh(newer)
        Server.delay = 0.04
        (self.base / "db/db.lck").touch()
        self.start()
        until = time.monotonic() + 20
        while time.monotonic() < until:
            state = self.state_value()
            if state.get("phase") == "downloading" and state.get("downloaded_bytes", 0) > 0:
                break
            time.sleep(0.02)
        else:
            raise AssertionError("no in-progress download to cancel")
        self.assert_sleep_inhibited()
        # A live Pacman transaction is protected; neither another planner nor
        # a package-removal request may delete its lock or modify the database.
        assert (self.base / "db/db.lck").exists()
        self.plan(expected=3)
        run([self.helper, "--remove-package", "flu-test-cancel"], expected=3)
        assert (self.base / "db/db.lck").exists()
        run([self.helper, "--cancel"])
        state = self.state_value()
        assert state["phase"] == "cancelled", state
        assert self.installed("flu-test-cancel") == "flu-test-cancel 1-1"
        assert not (self.base / "db/db.lck").exists()
        self.assert_sleep_released()
        Server.delay = 0
        self.reap()
        # Resume through the same public helper interface, using partial cache.
        self.start()
        state, _ = self.finish()
        assert state["phase"] == "complete", state
        assert self.installed("flu-test-cancel") == "flu-test-cancel 2-1"
        run([self.helper, "--cancel"], expected=4)
        return {"cancelled_without_installing": True, "resume_completed": True,
                "cancel_outside_download_rejected": True, "live_pacman_protected": True,
                "ownerless_stale_lock_cleared": True, "sleep_lock_released_on_cancel": True}

    def sleep_inhibition(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-sleep", "1-1")
        newer = package(repo, "flu-test-sleep", "2-1", payload=os.urandom(8 * 1024 * 1024))
        self.pacman("-U", "--noconfirm", initial)
        self.refresh(newer)
        Server.delay = 0.04
        self.start()
        until = time.monotonic() + 20
        while time.monotonic() < until:
            state = self.state_value()
            if state.get("phase") == "downloading" and state.get("downloaded_bytes", 0) > 0:
                break
            time.sleep(0.02)
        else:
            raise AssertionError(f"no protected download: {self.state_value()}")
        # Query the VM's real logind, not a fake D-Bus service. There is no UI
        # process in this scenario: the background worker owns the protection.
        self.assert_sleep_inhibited()
        worker_pid = int((self.base / "worker.pid").read_text())
        assert Path(f"/proc/{worker_pid}/exe").resolve() == self.worker
        # Only our isolated fixture process group is killed. Kernel descriptor
        # cleanup must release the inhibitor even without running Rust Drop.
        os.killpg(worker_pid, signal.SIGKILL)
        self.assert_sleep_released()
        # Reap Pacman as well as the worker before retrying. Closing the
        # worker's descriptor can precede delivery of SIGKILL to its child.
        until = time.monotonic() + 5
        while time.monotonic() < until:
            self.reap()
            try:
                os.killpg(worker_pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
        else:
            raise AssertionError("isolated worker process group did not exit")
        assert self.installed("flu-test-sleep") == "flu-test-sleep 1-1"
        Server.delay = 0
        self.start()
        state, _ = self.finish()
        assert state["phase"] == "complete", state
        assert self.installed("flu-test-sleep") == "flu-test-sleep 2-1"
        log = (self.base / "etc/flufflinux-update.log").read_text()
        assert log.index("[sleep inhibition acquired]") < log.index("[transaction preparation]")
        assert log.index("[installation]") < log.index("[sleep inhibition released]")
        return {"native_logind_block_sleep": True, "no_gui_required": True,
                "released_after_sigkill": True, "held_for_entire_update": True,
                "released_after_success": True}

    def sleep_inhibitor_failure(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-no-inhibitor", "1-1")
        newer = package(repo, "flu-test-no-inhibitor", "2-1")
        self.pacman("-U", "--noconfirm", initial)
        self.refresh(newer)
        before = (self.base / "pacman.log").read_bytes()
        # Use the standard D-Bus address environment, not a production test
        # switch. The missing socket simulates an unavailable system bus.
        self.state.write_text(json.dumps({"phase": "starting"}))
        run([self.worker], env={**os.environ,
            "DBUS_SYSTEM_BUS_ADDRESS": f"unix:path={self.base}/missing-bus"})
        state, _ = self.finish()
        assert state["phase"] == "failed", state
        assert state["error"] == "SLEEP_INHIBITOR_FAILED", state
        assert (self.base / "pacman.log").read_bytes() == before
        assert self.installed("flu-test-no-inhibitor") == "flu-test-no-inhibitor 1-1"
        assert not list((self.base / "cache").glob("flu-test-no-inhibitor-2-1*"))
        assert "[sleep inhibition failed]" in (self.base / "etc/flufflinux-update.log").read_text()
        return {"unavailable_logind_fails_closed": True, "pacman_not_started": True}

    def failed_download(self):
        repo = self.base / "repo"
        initial = package(repo, "flu-test-network", "1-1")
        newer = package(repo, "flu-test-network", "2-1")
        self.pacman("-U", "--noconfirm", initial)
        self.refresh(newer)
        Server.unavailable.add(newer.name)
        self.start()
        state, _ = self.finish()
        Server.unavailable.clear()
        assert state["phase"] == "failed", state
        assert state["error"] == "DOWNLOAD_CONNECTION_FAILED", state
        assert self.installed("flu-test-network") == "flu-test-network 1-1"
        assert not (self.base / "db/db.lck").exists()
        return {"error": state["error"], "installed_version_unchanged": True}

    def guards(self):
        invalid = run([self.helper, "/tmp/flufflinux-checkupdates-9999"], expected=2,
                      env={**os.environ, "PKEXEC_UID": "1000"})
        assert "invalid database path" in invalid.stdout
        self.policy.write_text("invalid JSON")
        repo = self.base / "repo"
        library = package(repo, "flu-test-guard", "1-1")
        newer = package(repo, "flu-test-guard", "2-1")
        blocker = package(repo, "flu-test-guard-blocker", "1-1", metadata=["depend = flu-test-guard=1-1"])
        self.pacman("-U", "--noconfirm", library, blocker)
        self.refresh(newer)
        result = self.plan(expected=20)
        assert "FLU_PROTECTED_REMOVAL:flu-test-guard-blocker" in result.stdout
        assert self.installed("flu-test-guard-blocker")
        return {"wrong_uid_database_rejected": True, "invalid_policy_fails_closed": True}


def child(args):
    # Private propagation is set before the first bind, including /tmp.
    run(["mount", "--make-rprivate", "/"])
    base = Path(tempfile.mkdtemp(prefix="flu-pacman-workflows-", dir=args.parent))
    tests = Workflows(base, args.helper, args.worker, args.controller)
    tests.selected_test = args.only
    success = False
    try:
        tests.test("update, install, replacement, download and progress", tests.replacement_and_update)
        for kind in ("protected", "warning", "autoremove"):
            tests.test(f"dependency conflict: {kind}", lambda kind=kind: tests.dependency_policy(kind))
        for kind in ("protected", "warning", "autoremove"):
            tests.test(f"direct package conflict: {kind}", lambda kind=kind: tests.direct_conflict_policy(kind))
        tests.test("mixed dependency safety priority", tests.mixed_dependency_policy)
        tests.test("unmanaged file conflict and preservation", tests.file_conflict)
        tests.test("package-owned file conflict fails safely", tests.owned_file_conflict)
        tests.test("download cancellation and resume", tests.cancellation)
        tests.test("download connection failure", tests.failed_download)
        tests.test("native sleep inhibition lifetime", tests.sleep_inhibition)
        tests.test("unavailable sleep inhibitor fails closed", tests.sleep_inhibitor_failure)
        tests.test("planner path and policy guards", tests.guards)
        tests.test("initial update timestamp", tests.record_current_update)
        if args.controller:
            tests.test("complete desktop controller workflow", tests.desktop_controller)
        success = True
    finally:
        report = {"passed": success, "fixture": str(base), "tests": tests.results}
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
        tests.server.shutdown()
        tests.reap()
    return 0 if success else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--controller", type=Path)
    parser.add_argument("--only", help="Run scenarios containing this text")
    parser.add_argument("--parent", type=Path, default=Path.cwd())
    parser.add_argument("--namespace-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.helper, args.worker = args.helper.resolve(strict=True), args.worker.resolve(strict=True)
    if args.controller:
        args.controller = args.controller.resolve(strict=True)
    args.output, args.parent = args.output.resolve(), args.parent.resolve(strict=True)
    if os.geteuid() != 0:
        parser.error("run with sudo on a disposable Linux development machine")
    if args.namespace_child:
        return child(args)
    env = {**os.environ, "FLU_HOST_MOUNT_NS": os.readlink("/proc/self/ns/mnt")}
    command = ["unshare", "--mount", "--pid", "--fork", "--kill-child", "--mount-proc",
               sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--namespace-child"]
    return subprocess.call(command, env=env)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        sys.exit(1)
