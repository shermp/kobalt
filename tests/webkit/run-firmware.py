"""Deployment diagnostics through the existing local Device API, never a build runner."""
from pathlib import Path
import argparse
import contextlib
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--virtualnickel', type=Path, required=True)
parser.add_argument('--epub-tests', action='store_true')
parser.add_argument('--epub-only', action='store_true')
parser.add_argument('--epub-corpus', action='store_true')
modes = parser.add_mutually_exclusive_group()
modes.add_argument('--allow-unsandboxed-experiment', action='store_true')
modes.add_argument('--allow-unsandboxed-device', action='store_true')
actions = parser.add_mutually_exclusive_group()
actions.add_argument('--platform-probe-only', action='store_true')
actions.add_argument('--font-resources-only', action='store_true')
args = parser.parse_args()
args.epub_tests |= args.epub_only or args.epub_corpus
if args.epub_tests and (args.platform_probe_only or args.font_resources_only):
    parser.error('Select EPUB rendering or a resource/platform diagnostic')
if not (args.platform_probe_only or args.font_resources_only) and not (args.allow_unsandboxed_experiment or args.allow_unsandboxed_device):
    parser.error('Rendering diagnostics require an explicit unsandboxed mode')
host_cmake = shutil.which('cmake')
if args.font_resources_only and not host_cmake:
    parser.error('Host CMake is required for diagnostic scratch cleanup')
root = Path(__file__).resolve().parents[2]
if args.epub_tests:
    fixture = root / 'tests/fixtures/trevelyans-type-tester.kepub.epub'
    with fixture.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == '1050bc7af6a6b19267319c8b56965ca7cbf63abedfa8079ab5c4eacbb6c407ac', 'General reading fixture changed'
corpus = []
if args.epub_corpus:
    references = json.loads((root / 'tests/fixtures/screenshot-books.json').read_text())
    for title in ('alice', 'moby-dick'):
        book = root / 'build/webkit/books' / references[title]['filename']
        with book.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == references[title]['sha256'], title
        corpus.append(book)
font_diagnostic = root / 'build/webkit/font-resources/webkit-font-resources'
if args.font_resources_only and not font_diagnostic.is_file():
    parser.error('Build the font-resources preset first')
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
    if args.epub_tests:
        shutil.copy2(Path(__file__).with_name('epub-client.py'), artifacts / 'epub-client.py')
        shutil.copy2(root / 'tests/fixtures/trevelyans-type-tester.kepub.epub', artifacts)
        if corpus:
            (artifacts / 'corpus').mkdir()
            for book in corpus:
                shutil.copy2(book, artifacts / 'corpus')
    prefix = '/mnt/onboard/.adds/kobalt-webkit'
    run(['podman', 'update', '--cpus', '1', container], stdout=subprocess.DEVNULL)
    run(['podman', 'exec', '--workdir', prefix, container, 'sha256sum', '-c', 'SHA256SUMS'],
        stdout=(output / 'package-check.log').open('w'))
    with (output / 'platform-probe.stderr').open('w') as error:
        probe = subprocess.run(
            ['podman', 'exec', container, '/usr/bin/vn-offline', '/usr/bin/qemu-arm',
             '-cpu', 'cortex-a7', '-L', '/firmware/rootfs', '-U', 'LD_PRELOAD',
             prefix + '/bin/kobalt-webkit-probe', '--context', 'qemu-user'],
            stdout=subprocess.PIPE, stderr=error, text=True, timeout=30)
    report = json.loads(probe.stdout)
    assert probe.returncode in (0, 77), (probe.returncode, report)
    assert report['context'] == 'qemu-user' and report['sandbox_verified'] is False
    missing = any(test['required'] and test['result'] != 'ok' for test in report['tests'])
    assert report['summary'] == ('unavailable' if missing else 'primitives-available')
    assert probe.returncode == (77 if missing else 0)
    (output / 'platform-probe.json').write_text(json.dumps(report, indent=2) + '\n')
    if args.font_resources_only:
        worker = install / 'kobalt-webkit/bin/kobalt-webkit-worker'
        backup = output / 'worker-backup'
        shutil.copy2(worker, backup)
        try:
            shutil.copy2(font_diagnostic, worker)
            with (output / 'font-resources.tsv').open('w') as log, (output / 'font-resources.stderr').open('w') as error:
                run(['podman', 'exec', container, '/usr/bin/vn-offline', '/usr/bin/qemu-arm',
                     '-cpu', 'cortex-a7', '-L', '/firmware/rootfs', '-U', 'LD_PRELOAD',
                     prefix + '/bin/kobalt-webkit', '--qemu-prefix', '/firmware/rootfs'],
                    stdout=log, stderr=error, timeout=45)
        finally:
            shutil.copy2(backup, worker)
            run([host_cmake, '-DPATHS=' + str(backup), '-P', str(root / 'platforms/cleanup.cmake')])
        run(['podman', 'exec', '--workdir', prefix, container, 'sha256sum', '-c', 'SHA256SUMS'],
            stdout=(output / 'package-restored.log').open('w'))
        paths = []
        for line in (output / 'font-resources.tsv').read_text().splitlines():
            fields = line.split('\t')
            if fields[0] in ('CONFIG', 'FONT'):
                paths.append(fields[1])
        assert paths, 'No font resources reported'
        verify = """import hashlib,json,sys
from pathlib import Path
paths=json.loads(sys.argv[1])
records=[]
for name in sorted(set(paths)):
    path=Path(name).resolve(strict=True)
    assert path.is_relative_to('/firmware/rootfs'), (name,str(path))
    digest=None
    if path.is_file():
        with path.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
    records.append(dict(path=name,resolved=str(path),kind='directory' if path.is_dir() else 'file',sha256=digest))
print(json.dumps(records,indent=2))
"""
        with (output / 'font-provenance.json').open('w') as log:
            run(['podman', 'exec', container, 'python3', '-c', verify, json.dumps(paths)], stdout=log)
    if args.platform_probe_only or args.font_resources_only:
        assert device.command('ping')['status'] == 'dispatched'
        (output / 'nickel.json').write_text(json.dumps(dict(responsive=True)))
        print('WEBKIT_FONT_RESOURCES_VERIFIED' if args.font_resources_only else 'WEBKIT_PLATFORM_PROBE_VERIFIED', flush=True)
        sys.exit(0)
    if not args.epub_only:
        with (output / 'client.log').open('w') as log:
            run(['podman', 'exec', container, 'python3', '/instance/artifacts/webkit-client.py',
                 prefix, '/instance/artifacts/webkit-results',
                 'device-unsandboxed' if args.allow_unsandboxed_device else 'experiment'],
                stdout=log, stderr=subprocess.STDOUT, timeout=240)
        shutil.copytree(artifacts / 'webkit-results', output / 'results')
    if args.epub_tests:
        with (output / 'epub-client.log').open('w') as log:
            command = ['podman', 'exec', container, 'python3', '/instance/artifacts/epub-client.py',
                 prefix, '/instance/artifacts/epub-results',
                 'device-unsandboxed' if args.allow_unsandboxed_device else 'experiment']
            if corpus:
                command.append('/instance/artifacts/corpus')
            run(command, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        shutil.copytree(artifacts / 'epub-results', output / 'epub-results')
    assert device.command('ping')['status'] == 'dispatched'
    (output / 'nickel.json').write_text(json.dumps(dict(responsive=True)))
    print('WEBKIT_EPUB_FIRMWARE_VERIFIED' if args.epub_tests else 'WEBKIT_M1_VERIFIED', flush=True)
finally:
    try:
        for source, target in (('webkit-results', 'results'), ('epub-results', 'epub-results')):
            if (device.folder / 'artifacts' / source).exists() and not (output / target).exists():
                shutil.copytree(device.folder / 'artifacts' / source, output / target)
    finally:
        try:
            device.close()
        finally:
            run([str(vn / 'launch.sh'), '--delete', '--name', name],
                env=dict(os.environ, CONTAINER_ENGINE='podman'), stdout=subprocess.DEVNULL, timeout=120)
            assert not device.folder.exists(), 'Owned device directory remains'
            volumes = subprocess.check_output(['podman', 'volume', 'ls', '-q', '--filter',
                                              'name=virtualnickel-system-' + name], text=True, timeout=15)
            assert not volumes.strip(), 'Owned system volume remains'
            print('DEVICE_DELETED ' + name, flush=True)
