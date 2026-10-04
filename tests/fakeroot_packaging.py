#!/usr/bin/env python3
"""Test the public make fakeroot entry point using a tiny CMake-only payload."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class Packaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="flu packaging test ")
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name) / "project with spaces"
        self.source.mkdir()
        (self.source / "cmake").mkdir()
        for name in ("Makefile", ".PKGINFO", "cmake/StagePackage.cmake"):
            shutil.copyfile(SOURCE / name, self.source / name)
        self.metadata = (self.source / ".PKGINFO").read_text()
        self.version = next(line.removeprefix("pkgver = ") for line in self.metadata.splitlines()
                            if line.startswith("pkgver = "))
        (self.source / "payload").write_bytes(b"built package payload\n")
        (self.source / "policy").write_bytes(b'{"protected":[]}\n')
        self.project = '''cmake_minimum_required(VERSION 3.24)
project(packaging_fixture NONE)
add_custom_target(payload ALL
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/payload"
        "${CMAKE_CURRENT_BINARY_DIR}/compiled")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/compiled" DESTINATION share/flu-fakeroot-test)
install(FILES policy DESTINATION /etc/pacman.d)
'''
        (self.source / "CMakeLists.txt").write_text(self.project)
        self.output = self.source / "fakeroot"

    def run_make(self, success=True, *options):
        env = {**os.environ, "SOURCE_DATE_EPOCH": "1700000000"}
        # An inherited DESTDIR must never redirect or nest the requested stage.
        env["DESTDIR"] = str(self.source / "unexpected-destdir")
        result = subprocess.run(
            [MAKE, "-j2", "fakeroot", f"CMAKE={CMAKE}",
             "BUILD_DIR=build with spaces", "JOBS=2", *options],
            cwd=self.source, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=60,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout)
        return result.stdout

    def assert_metadata(self, version=None):
        result = (self.output / ".PKGINFO").read_text()
        expected_size = sum(path.stat().st_size for path in self.output.rglob("*")
                            if path.is_file() and path.name != ".PKGINFO")
        self.assertIn(f"pkgver = {version or self.version}\n", result)
        self.assertIn("builddate = 1700000000\n", result)
        self.assertIn(f"size = {expected_size}\n", result)
        self.assertIn("depend = kirigami\n", result)
        self.assertEqual((self.output / "usr/share/flu-fakeroot-test/compiled").read_bytes(),
                         (self.source / "payload").read_bytes())
        self.assertTrue((self.output / "etc/pacman.d/policy").is_file())
        self.assertFalse((self.source / "unexpected-destdir").exists())
        self.assertEqual(list(self.source.glob(".fakeroot-*")), [])

    def test_build_install_metadata_and_source_preservation(self):
        self.run_make()
        self.assert_metadata()
        self.assertEqual((self.source / ".PKGINFO").read_text(), self.metadata)

    def test_restage_rebuilds_payload_and_removes_obsolete_files(self):
        self.run_make()
        (self.output / "obsolete").write_text("must not leak into the next package")
        (self.source / "payload").write_bytes(b"new compiled payload, larger than before\n")
        (self.source / ".PKGINFO").write_text(
            self.metadata.replace(f"pkgver = {self.version}", "pkgver = 0.0.42-9"))
        self.run_make()
        self.assert_metadata("0.0.42-9")
        self.assertFalse((self.output / "obsolete").exists())

    def test_install_failure_preserves_previous_output(self):
        self.run_make()
        (self.source / "CMakeLists.txt").write_text(
            self.project + '\ninstall(CODE "message(FATAL_ERROR \\"deliberate install failure\\")")\n')
        self.run_make(False)
        self.assert_metadata()

    def test_build_failure_preserves_previous_output(self):
        self.run_make()
        (self.source / "CMakeLists.txt").write_text(
            self.project + '\nadd_custom_target(fail ALL COMMAND "${CMAKE_COMMAND}" -E false)\n')
        self.run_make(False)
        self.assert_metadata()

    def test_symlink_destination_is_not_followed(self):
        outside = Path(self.temporary.name) / "personal files"
        outside.mkdir()
        (outside / "keep").write_text("unchanged")
        self.output.symlink_to(outside, target_is_directory=True)
        self.assertIn("symlink", self.run_make(False))
        self.assertEqual((outside / "keep").read_text(), "unchanged")
        self.assertFalse((self.source / "build with spaces").exists())

    def test_invalid_metadata_preserves_previous_output(self):
        self.run_make()
        for contents in (self.metadata + "size = 123\n", self.metadata.replace("size = ", "size = invalid")):
            (self.source / ".PKGINFO").write_text(contents)
            self.run_make(False)
            self.assert_metadata()
        (self.source / ".PKGINFO").unlink()
        self.run_make(False)
        self.assert_metadata()

    def test_build_directory_cannot_be_inside_staging(self):
        self.output.mkdir()
        (self.output / "keep").write_text("unchanged")
        for directory in ("fakeroot/build", "."):
            self.assertIn("BUILD_DIR", self.run_make(False, f"BUILD_DIR={directory}"))
        self.assertEqual((self.output / "keep").read_text(), "unchanged")
        self.assertFalse((self.output / "build").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    SOURCE, CMAKE, MAKE = args.source.resolve(), args.cmake, args.make
    unittest.main(argv=[__file__], verbosity=2)
