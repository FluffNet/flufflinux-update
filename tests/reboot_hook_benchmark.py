#!/usr/bin/env python3
"""Compare real pre/post-transaction hooks without changing the live reboot state."""
import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--namespace-child', action='store_true')
args = parser.parse_args()
assert os.geteuid() == 0, 'run on the disposable VM with sudo'
if not args.namespace_child:
    env = {**os.environ, 'FLU_BENCH_HOST_NS': os.readlink('/proc/self/ns/mnt')}
    raise SystemExit(subprocess.call(['unshare', '--mount', '--fork', sys.executable,
                                      __file__, *sys.argv[1:], '--namespace-child'], env=env))
assert os.readlink('/proc/self/ns/mnt') != os.environ['FLU_BENCH_HOST_NS']
subprocess.run(['mount', '--make-rprivate', '/'], check=True)
with tempfile.TemporaryDirectory(prefix='flu-hook-benchmark-') as temporary:
    private = Path(temporary) / 'pacman.d'
    # Read-only ownership queries need repository configuration, not keyring sockets.
    shutil.copytree('/etc/pacman.d', private, symlinks=True, ignore=shutil.ignore_patterns('gnupg'))
    subprocess.run(['mount', '--bind', private, '/etc/pacman.d'], check=True)
    samples = {'baseline': [], 'candidate': []}
    post_samples = {'baseline': [], 'candidate': []}
    expected = None
    try:
        for _ in range(5):
            for name, binary in [('baseline', args.baseline), ('candidate', args.candidate)]:
                start = time.perf_counter()
                result = subprocess.run([binary, '--reboot-before'], capture_output=True, text=True, check=True)
                samples[name].append(time.perf_counter() - start)
                assert not result.stderr, result.stderr
                snapshot = json.loads((private / 'flufflinux-update-reboot-before.json').read_text())
                comparable = {field: snapshot[field] for field in ['boot_id', 'watched', 'versions']}
                if expected is None:
                    expected = comparable
                assert comparable == expected, 'optimized hook changed the captured restart policy'
                marker = private / 'flufflinux-update-reboot.json'
                before_marker = marker.read_bytes() if marker.exists() else None
                start = time.perf_counter()
                result = subprocess.run([binary, '--reboot-after'], capture_output=True, text=True, check=True)
                post_samples[name].append(time.perf_counter() - start)
                assert not result.stderr, result.stderr
                assert not (private / 'flufflinux-update-reboot-before.json').exists()
                assert (marker.read_bytes() if marker.exists() else None) == before_marker

        report = {'loaded_modules': len(Path('/proc/modules').read_text().splitlines()),
                  'identical_snapshots': True, 'watched_packages': len(expected['watched']),
                  'samples_seconds': samples,
                  'median_seconds': {name: statistics.median(values) for name, values in samples.items()}}
        report['post_samples_seconds'] = post_samples
        report['post_median_seconds'] = {name: statistics.median(values) for name, values in post_samples.items()}
        report['speedup'] = report['median_seconds']['baseline'] / report['median_seconds']['candidate']
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    finally:
        subprocess.run(['umount', '/etc/pacman.d'], check=True)
