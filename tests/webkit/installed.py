"""Installed-mod diagnostics through the existing Device API; no build logic."""
import json
from pathlib import PurePosixPath
import shutil
import subprocess
import zipfile


def engine_command(output, mode):
    """Configure only this owned Nickel launch using the engine override."""
    executable = shutil.which('podman')
    assert executable, 'Podman is required for firmware diagnostics'
    environment = {
        'KOBALT_READER_BACKEND': 'webkit',
        'KOBALT_WEBKIT_LAUNCHER': '/instance/onboard/.adds/kobalt-webkit/bin/kobalt-webkit',
        'KOBALT_WEBKIT_MODE': mode,
        'KOBALT_WEBKIT_QEMU_PREFIX': '/firmware/rootfs',
        'QEMU_LD_PREFIX': '/firmware/rootfs',
        'KOBALT_DIAGNOSTICS': '1',
        'KOBALT_STRICT_MAIN_THREAD': '1',
    }
    command = output / 'podman-installed'
    command.write_text('''#!/usr/bin/python3
import os, sys
arguments = sys.argv[1:]
if arguments and arguments[0] == 'run' and '/usr/local/Kobo/nickel' in arguments:
    index = arguments.index('/usr/local/Kobo/nickel')
    assert 'qemu-arm' in arguments[:index], 'Expected the existing ARM runtime command'
    extra = []
    for key, value in ENVIRONMENT.items():
        extra += ['-E', key + '=' + value]
    arguments[index:index] = extra
    # QEMU prefixes an existing source but not a nonexistent destination.
    # Two onboard bind mounts then make atomic link/rename report EXDEV.
    # This fresh test device uses one bind mount and a guest-root symlink.
    mounts = [i for i, value in enumerate(arguments)
              if value.endswith(':/firmware/rootfs/mnt/onboard:rw,z')]
    assert len(mounts) == 1 and arguments[mounts[0] - 1] == '-v'
    del arguments[mounts[0] - 1:mounts[0] + 1]
    index = arguments.index('/usr/bin/vn-offline')
    original = arguments[index + 1:]
    setup = ('if [ -L /firmware/rootfs/mnt/onboard ]; then '
             '[ "$(readlink /firmware/rootfs/mnt/onboard)" = /mnt/onboard ]; '
             'else rmdir /firmware/rootfs/mnt/onboard; '
             'ln -s /mnt/onboard /firmware/rootfs/mnt/onboard; fi; exec "$@"')
    arguments[index:] = ['/usr/bin/vn-offline', 'sh', '-ec', setup, 'sh', *original]
os.execv(EXECUTABLE, [EXECUTABLE, *arguments])
'''.replace('ENVIRONMENT', repr(environment)).replace('EXECUTABLE', repr(executable)))
    command.chmod(0o755)
    return str(command)


def prepare(root, device, package):
    with zipfile.ZipFile(package) as archive:
        for member in archive.infolist():
            name = PurePosixPath(member.filename)
            assert not name.is_absolute() and '..' not in name.parts, member.filename
            if member.is_dir() or not name.parts or name.parts[0] not in ('.adds', '.kobo'):
                continue
            target = device.onboard / member.filename
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(member))
    shutil.copy2(root / 'tests/fixtures/trevelyans-type-tester.kepub.epub',
                 device.onboard / 'trevelyans-type-tester.kepub.epub')
    profile = device.onboard / '.adds/kobalt'
    profile.mkdir(parents=True, exist_ok=True)
    (profile / 'diagnostics').mkdir(exist_ok=True)
    (profile / 'config.json').write_text(json.dumps({'enabled': True}))
    (profile / 'reader-settings.json').write_text(json.dumps({
        'schema': 1, 'sleep_screen': 'page', 'sleep_tile': 'none', 'auto_rotate': True}))
    (profile / 'diagnostics-enabled').touch()
    (profile / 'live-state').touch()


def check(device, output):
    from control_stream import read_json
    profile = device.onboard / '.adds/kobalt'
    state_file = profile / 'diagnostics/state.json'
    session_file = profile / 'diagnostics/session.json'
    results = output / 'installed-results'
    results.mkdir()
    transcript = []
    current_page = None

    def events():
        log = profile / 'diagnostics/events.jsonl'
        if not log.exists():
            return []
        records = []
        for line in log.read_bytes().splitlines(keepends=True):
            # A logging-worker append may still be in progress.
            if line.endswith(b'\n'):
                records.append(json.loads(line))
        return records

    def cursor():
        records = events()
        return max((entry.get('monotonic_ms', 0) for entry in records), default=0)

    def event(name, after, timeout=120):
        def ready():
            found = [entry for entry in events() if entry.get('event') == name
                     and entry.get('monotonic_ms', 0) > after]
            return found[-1] if found else None
        return device.wait_until(ready, timeout=timeout, description=name)

    def state(predicate=lambda value: True, description='reader frame', timeout=120):
        def ready():
            value = read_json(state_file)
            return value if value and not value.get('busy', True) and predicate(value) else None
        return device.wait_until(ready, timeout=timeout, description=description)

    def capture(name):
        device.run({'action': 'settle', 'quiet_ms': 250, 'timeout_ms': 10000})
        device.capture(name, raw=True)
        for suffix in ('.bmp', '.json'):
            shutil.copy2(device.folder / 'artifacts' / (name + suffix), results)

    def call(action, **values):
        answer = device.command(action, **values)
        assert answer['status'] == 'dispatched', answer
        return answer

    def open_book(backend, expected_page=None):
        nonlocal current_page
        previous = state_file.stat().st_mtime_ns if state_file.exists() else 0
        device.tap_widget(class_name='BookCoverView')
        device.wait_widget(name='rrPage', timeout=240)
        value = state(lambda item: state_file.stat().st_mtime_ns > previous,
                      description=backend + ' installed reader frame', timeout=240)
        assert value.get('backend', 'crengine') == backend, value
        if backend == 'webkit' and expected_page is None:
            assert value['chapter_title'] == 'Title Page', value['chapter_title']
            assert value['chapter_page'] == 1, value['chapter_page']
        if expected_page is not None:
            assert value['page'] == expected_page, (value['page'], expected_page)
        assert value.get('pages', 0) > 1, value
        current_page = value['page']
        transcript.append({'action': 'open', 'backend': backend, 'state': value})
        return value

    def contents(backend):
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        device.wait_widget(name='rrNavChapters')
        capture(backend + '-chapters-menu')
        device.tap_widget(name='rrNavChapters')
        device.wait_widget(name='rrChapter0')
        capture(backend + '-contents')
        before = cursor()
        device.tap_widget(name='rrChapter0')
        event('frame-ready', before)
        device.wait_widget(name='rrPage')

    def turn(direction=1):
        nonlocal current_page
        before = current_page
        after = cursor()
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + int(paper['width'] * (0.9 if direction > 0 else 0.1)),
             y=paper['y'] + paper['height'] // 2)
        value = event('page-turn-painted', after)
        assert value['page'] != before, value
        current_page = value['page']
        transcript.append({'action': 'turn', 'state': value})
        return value

    def close_book(menu_capture=None):
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        if menu_capture:
            device.wait_widget(name='rrReturn')
            capture(menu_capture)
        device.tap_widget(name='rrReturn')
        device.wait_until(lambda: read_json(session_file).get('status') == 'closed',
                          timeout=60, description='installed reader close')
        assert not (profile / 'active-session.json').exists()
        remaining = subprocess.check_output(['podman', 'exec', 'virtualnickel-' + device.name,
            'find', '/dev/shm', '-maxdepth', '1', '-name', 'kobalt-webkit-*'], text=True)
        assert not remaining.strip(), remaining
        transcript.append({'action': 'close', 'helper_cleanup': True})

    try:
        device.wait_log('reader: native controller ready', timeout=120)
        capture('library')
        first = open_book('webkit')
        capture('webkit-open')
        assert first['chapters'] > 0, first
        contents('webkit')
        forward = turn()
        assert forward['page'] > first['page']
        capture('webkit-next')
        backward = turn(-1)
        assert backward['page'] == first['page']
        # The installed UI and helper must survive both geometry and power
        # transitions. Report markers are independent of persistent positions.
        before = cursor()
        call('rotate')
        event('frame-ready', before)
        device.wait_until(lambda: any(w['name'] == 'rrPage' and w['width'] > w['height']
                          for w in device.ui()['widgets']), timeout=60,
                          description='installed landscape viewport')
        capture('webkit-landscape')
        # Continue through the rotation cycle to ordinary portrait.
        for _ in range(3):
            before = cursor()
            call('rotate')
            event('frame-ready', before)
        device.wait_until(lambda: any(w['name'] == 'rrPage' and w['height'] > w['width']
                          for w in device.ui()['widgets']), timeout=60,
                          description='installed portrait viewport')
        before = cursor()
        call('power')
        event('reader-hidden', before)
        device.wait_until(lambda: not subprocess.check_output([
            'podman', 'exec', 'virtualnickel-' + device.name, 'find', '/dev/shm',
            '-maxdepth', '1', '-name', 'kobalt-webkit-*'], text=True).strip(),
            timeout=30, description='helper stopped for sleep')
        sleeping = state()
        assert sleeping['backend'] == 'webkit'
        capture('webkit-sleep')
        before = cursor()
        call('power')
        event('reader-visible', before)
        device.wait_widget(name='rrPage', timeout=60)
        turn()
        capture('webkit-wake')
        saved_webkit_page = current_page
        close_book('webkit-menu')
        capture('library-after-close')
        # Only this disposable device's helper is hidden to exercise the public
        # opening fallback. Restore its exact package path in finally.
        launcher = device.onboard / '.adds/kobalt-webkit/bin/kobalt-webkit'
        hidden = launcher.with_suffix('.held')
        launcher.rename(hidden)
        try:
            open_book('crengine')
            turn()
            capture('crengine-fallback')
            contents('crengine')
            close_book('crengine-menu')
        finally:
            hidden.rename(launcher)
        open_book('webkit', expected_page=saved_webkit_page)
        capture('webkit-restored')
        turn()
        close_book()
        assert call('ping')['status'] == 'dispatched'
        records = events()
        blocked = [entry for entry in records if entry.get('event') == 'main-thread-blocked']
        assert not blocked, blocked
        report = {'status': 'passed', 'installed_mod': True, 'webkit_navigation': True,
                  'rotation': True, 'sleep_resume': True,
                  'creengine_fallback': True, 'reopen': True, 'helper_cleanup': True,
                  'persistent_position': True, 'toc_navigation': True,
                  'main_thread_blocked': blocked, 'transcript': transcript}
        (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
        print('WEBKIT_INSTALLED_MOD_VERIFIED', flush=True)
    finally:
        try:
            (results / 'final-ui.json').write_text(json.dumps(device.ui(), indent=2) + '\n')
            device.capture('installed-final', raw=True)
            shutil.copy2(device.folder / 'artifacts/installed-final.bmp', results)
        except Exception as error:
            (results / 'capture-error.txt').write_text(str(error))
        if (profile / 'diagnostics').is_dir():
            shutil.copytree(profile / 'diagnostics', results / 'diagnostics', dirs_exist_ok=True)
        for source in device.folder.glob('artifacts/nickel*.log'):
            shutil.copy2(source, results)
