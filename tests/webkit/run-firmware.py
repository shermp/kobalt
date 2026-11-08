"""Deployment diagnostics through the existing local Device API, never a build runner."""
from pathlib import Path
import argparse
import contextlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--virtualnickel', type=Path, required=True)
parser.add_argument('--allow-unsandboxed-experiment', action='store_true', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
vn = args.virtualnickel.resolve()
sys.path.insert(0, str(vn / 'tools'))
from device import Device

bundle = Path((root / 'build/webkit/bundle.txt').read_text().strip().replace('/work/', str(root) + '/', 1))
archive = bundle.parent / 'kobalt-webkit-armhf.tar'
assert archive.is_file(), 'Build the ARM package first'
output = root / 'build/webkit' / ('firmware-' + uuid.uuid4().hex[:10])
output.mkdir()
name = 'agent_kobalt_wpe_m1_' + uuid.uuid4().hex[:10]
device = Device(name, engine='podman')
container = 'virtualnickel-' + name


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=kwargs.pop('timeout', 180), **kwargs)


try:
    print('RESULTS ' + str(output), flush=True)
    with (output / 'prepare.log').open('w') as log, contextlib.redirect_stdout(log):
        device.prepare(device='libra-colour', firmware='6.0.274403')
        device.start(device='libra-colour', firmware='6.0.274403', wait=False, memory_mib=768)
        device.wait_library_ready(timeout=120)
    print('DEVICE_READY ' + name, flush=True)
    install = device.onboard / '.adds'
    install.mkdir(exist_ok=True)
    with tarfile.open(archive) as package:
        package.extractall(install, filter='data')
    artifacts = device.folder / 'artifacts'
    shutil.copy2(Path(__file__).with_name('firmware-client.py'), artifacts / 'webkit-client.py')
    prefix = '/mnt/onboard/.adds/kobalt-webkit'
    run(['podman', 'update', '--cpus', '1', container], stdout=subprocess.DEVNULL)
    run(['podman', 'exec', '--workdir', prefix, container, 'sha256sum', '-c', 'SHA256SUMS'],
        stdout=(output / 'package-check.log').open('w'))
    with (output / 'client.log').open('w') as log:
        run(['podman', 'exec', container, 'python3', '/instance/artifacts/webkit-client.py',
             prefix, '/instance/artifacts/webkit-results'],
            stdout=log, stderr=subprocess.STDOUT, timeout=240)
    shutil.copytree(artifacts / 'webkit-results', output / 'results')
    assert device.command('ping')['status'] == 'dispatched'
    (output / 'nickel.json').write_text(json.dumps(dict(responsive=True)))
    print('WEBKIT_M1_VERIFIED', flush=True)
finally:
    try:
        if (device.folder / 'artifacts/webkit-results').exists() and not (output / 'results').exists():
            shutil.copytree(device.folder / 'artifacts/webkit-results', output / 'results')
        device.close()
    finally:
        run([str(vn / 'launch.sh'), '--delete', '--name', name],
            env=dict(os.environ, CONTAINER_ENGINE='podman'), stdout=subprocess.DEVNULL, timeout=120)
        assert not device.folder.exists(), 'Owned device directory remains'
        volumes = subprocess.check_output(['podman', 'volume', 'ls', '-q', '--filter',
                                          'name=virtualnickel-system-' + name], text=True, timeout=15)
        assert not volumes.strip(), 'Owned system volume remains'
        print('DEVICE_DELETED ' + name, flush=True)
