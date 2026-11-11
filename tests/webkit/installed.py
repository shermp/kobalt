"""Installed-mod diagnostics through the existing Device API; no build logic."""
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
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


def prepare(root, device, package, preview_menu=False):
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
    settings = {'schema': 1, 'sleep_screen': 'page', 'sleep_tile': 'none', 'auto_rotate': True}
    if preview_menu:
        settings['default_menu_view'] = 'none'
    (profile / 'reader-settings.json').write_text(json.dumps(settings))
    (profile / 'diagnostics-enabled').touch()
    (profile / 'live-state').touch()


def check(device, output, search_only=False, rotate_after_search=False, bookmarks_only=False,
          annotations_only=False, recovery_only=False, previews_only=False):
    from control_stream import read_json
    profile = device.onboard / '.adds/kobalt'
    state_file = profile / 'diagnostics/state.json'
    session_file = profile / 'diagnostics/session.json'
    results = output / 'installed-results'
    results.mkdir()
    transcript = []
    current_page = None
    memory_samples = []
    memory_root = None

    def memory_sample(phase):
        nonlocal memory_root
        container = 'virtualnickel-' + device.name
        sample = {'phase': phase}
        inspected = subprocess.run(['podman', 'inspect', '--format', '{{json .State}}', container],
                                   capture_output=True, text=True)
        try:
            sample['container'] = json.loads(inspected.stdout)
            pid = sample['container'].get('Pid', 0)
            if pid:
                group = (Path('/proc') / str(pid) / 'cgroup').read_text().splitlines()
                unified = next(line.split(':', 2)[2] for line in group if line.startswith('0::'))
                memory_root = Path('/sys/fs/cgroup') / unified.lstrip('/')
            if memory_root:
                sample['cgroup'] = {name: (memory_root / ('memory.' + name)).read_text().strip()
                                   for name in ('current', 'peak', 'max', 'events', 'stat')}
        except (OSError, ValueError, StopIteration) as error:
            sample['collection_error'] = str(error)
        processes = subprocess.run(['podman', 'top', container, 'hpid,pid,ppid,rss,comm'],
                                   capture_output=True, text=True)
        sample['processes'] = processes.stdout
        memory_samples.append(sample)
        (results / 'memory.json').write_text(json.dumps(memory_samples, indent=2) + '\n')

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
        memory_sample(name)

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
        arrived = event('frame-ready', before)
        device.wait_widget(name='rrPage')
        return arrived

    def search(backend):
        nonlocal current_page
        original = state(description='source position before search')
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        device.wait_widget(name='rrSearchOpen')
        capture(backend + '-search-menu')
        device.tap_widget(name='rrSearchOpen')
        device.wait_widget(name='rrSearchQuery')
        capture(backend + '-search-entry')
        (results / (backend + '-search-ui.json')).write_text(json.dumps(device.ui(), indent=2))
        # Firmware 6's VirtualKey exposes no text property. Use the observed
        # English keyboard rows from this pinned firmware, retaining its UI
        # snapshot and screenshot as evidence rather than injecting text.
        keys = [widget for widget in device.ui()['widgets'] if widget['class'] == 'VirtualKey'
                and any(parent.get('name') == 'rrSearchKeyboard'
                        for parent in widget.get('ancestors', []))]
        rows = sorted({widget['y'] for widget in keys})
        assert len(rows) == 5, rows
        letters = {}
        for y, alphabet in zip(rows[1:3], ('qwertyuiop', 'asdfghjkl@')):
            row = sorted((widget for widget in keys if widget['y'] == y), key=lambda item: item['x'])
            assert len(row) == len(alphabet), row
            letters.update(zip(alphabet, row))
        typed = ''
        for letter in 'typography':
            button = letters[letter]
            call('tap', x=button['x'] + button['width'] // 2,
                 y=button['y'] + button['height'] // 2)
            typed += letter
            device.wait_widget(name='rrSearchQuery', text=typed)
        device.tap_widget(name='rrSearchSubmit')
        device.wait_widget(name='rrSearchResult0', timeout=240)
        device.wait_until(lambda: any(widget.get('name') == 'rrSearchStatus'
                         and 'Searching' not in widget.get('text', '')
                         for widget in device.ui()['widgets']),
                         timeout=240, description='source search completion')
        device.wait_widget(name='rrSearchStatus', text='3 matches')
        capture(backend + '-search-results')
        before = cursor()
        device.tap_widget(name='rrSearchResult0')
        event('frame-ready', before)
        value = state(description='search result source destination')
        current_page = value['page']
        assert value.get('backend', 'crengine') == backend, value
        capture(backend + '-search-destination')
        transcript.append({'action': 'search', 'backend': backend, 'state': value})
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        device.tap_widget(name='rrSearchOpen')
        device.wait_widget(name='rrSearchOriginal')
        capture(backend + '-search-return')
        before = cursor()
        device.tap_widget(name='rrSearchOriginal')
        event('frame-ready', before)
        returned = state(description='return to original search position')
        assert returned['page'] == original['page'], (original, returned)
        if backend == 'webkit':
            assert returned['source_position'] == original['source_position'], (original, returned)
        current_page = returned['page']

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

    def rotations():
        memory_sample('before-rotations')
        for index in range(4):
            before = cursor()
            call('rotate')
            event('frame-ready', before)
            device.wait_until(lambda: any(w['name'] == 'rrPage' and
                              ((w['width'] > w['height']) == (index % 2 == 0))
                              for w in device.ui()['widgets']), timeout=60,
                              description='installed rotated viewport')
            memory_sample('rotation-' + str(index + 1))
            if index == 0:
                capture('webkit-landscape')

    def open_marks():
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        device.tap_widget(name='rrBookmarks')
        device.wait_widget(name='rrBookmarkList')

    def close_marks():
        device.tap_widget(name='rrBookmarkListClose')
        device.wait_until(lambda: not any(w['name'] == 'rrBookmarkList' for w in device.ui()['widgets']),
                          description='bookmark panel closed')
        if any(w['name'] == 'rrBookmarks' for w in device.ui()['widgets']):
            paper = device.wait_widget(name='rrPage')
            call('tap', x=paper['x'] + 20, y=paper['y'] + paper['height'] // 2)
            device.wait_until(lambda: not any(w['name'] == 'rrBookmarks' for w in device.ui()['widgets']),
                              description='bookmark menu dismissed')

    def bookmark(backend):
        nonlocal current_page
        original = state(description='position before source bookmark')
        paper = device.wait_widget(name='rrPage')
        before = cursor()
        call('tap', x=paper['x'] + int(paper['width'] * 0.835), y=paper['y'] + 60)
        saved = event('bookmarks-saved', before)
        assert not saved.get('failed', False) and saved['count'] == 1, saved
        # Adding a ribbon need not publish another reader frame or state dump.
        # The save event and visible list establish the edit before the next turn.
        capture(backend + '-bookmark-page')
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        device.wait_widget(name='rrBookmarks')
        capture(backend + '-bookmark-menu')
        device.tap_widget(name='rrBookmarks')
        device.wait_widget(name='rrBookmark0')
        capture(backend + '-bookmark-list')
        close_marks()
        departure = turn()
        open_marks()
        before = cursor()
        device.tap_widget(name='rrBookmark0')
        arrived = event('frame-ready', before)
        assert arrived['page'] == original['page'], (arrived, original)
        capture(backend + '-bookmark-destination')
        paper = device.wait_widget(name='rrPage')
        call('tap', x=paper['x'] + paper['width'] // 2,
             y=paper['y'] + paper['height'] // 2)
        returning = device.wait_until(lambda: next((w for w in device.ui()['widgets']
                  if w['name'] in ('rrLinkReturn', 'rrScrubberReturn')), None),
                  description='bookmark return action')
        before = cursor()
        device.tap_widget(name=returning['name'])
        returned = event('frame-ready', before)
        assert returned['page'] == departure['page'], (returned, departure)
        current_page = returned['page']
        transcript.append({'action': 'bookmark', 'backend': backend, 'source': original,
                           'returned': returned})

    def annotation_list(expected):
        open_marks()
        device.tap_widget(name='rrAnnotationsTab')
        if expected:
            device.wait_widget(name='rrAnnotation0')
            device.wait_widget(text=expected, ancestor_name='rrAnnotation0')
            assert not any(w['name'] == 'rrAnnotation1' for w in device.ui()['widgets'])
        else:
            device.wait_widget(name='rrBookmarkListEmptyHint')
            assert not any(widget['name'] == 'rrAnnotation0' for widget in device.ui()['widgets'])

    def annotate(backend):
        paper = device.wait_widget(name='rrPage')
        assert (paper['width'], paper['height']) == (1264, 1680), paper
        call('tap', x=paper['x'] + paper['width'] // 2, y=paper['y'] + paper['height'] // 2)
        device.wait_widget(name='rrAnnotate')
        capture(backend + '-annotate-entry')
        device.tap_widget(name='rrAnnotate')
        device.wait_until(lambda: not any(w['name'] in ('rrAnnotate', 'rrAnnotationModeNotice')
                                          for w in device.ui()['widgets']),
                          description='highlight mode indicator dismissed')
        device.run({'action': 'settle', 'quiet_ms': 250, 'timeout_ms': 10000})
        # These points hit Type and the space beyond Tester on the pinned
        # Trevelyan title page. Each renderer lays out the title independently.
        x, y, beyond = (435, 434, 1010) if backend == 'webkit' else (415, 505, 1025)
        frame = device.folder / 'artifacts/nickel-main.bmp'
        gesture = backend + '-selection'
        before = frame.read_bytes()
        call('pointer', phase='down', gesture=gesture, x=x, y=y)
        device.wait_until(lambda: frame.read_bytes() != before, timeout=4,
                          description='long press selection painted')
        before = frame.read_bytes()
        call('pointer', phase='move', gesture=gesture, x=beyond, y=y)
        device.wait_until(lambda: frame.read_bytes() != before, timeout=4,
                          description='extended selection painted')
        call('pointer', phase='up', gesture=gesture, x=beyond, y=y)
        capture(backend + '-selection-draft')
        call('tap', x=x, y=y)
        device.wait_widget(name='rrAnnotationSave')
        capture(backend + '-selection-choices')
        device.tap_widget(name='rrAnnotationSave')
        device.wait_until(lambda: not any(w['name'] == 'rrAnnotationPopover'
                                          for w in device.ui()['widgets']),
                          description='mark choices dismissed')
        annotation_list('Type Tester')
        capture(backend + '-annotation-list')
        close_marks()
        capture(backend + '-annotation-saved')
        transcript.append({'action': 'annotate', 'backend': backend, 'excerpt': 'Type Tester'})

    def adjust_source_annotation():
        gesture = 'saved-selection'
        call('pointer', phase='down', gesture=gesture, x=435, y=434)
        device.wait_widget(name='rrAnnotationEdit', timeout=4)
        call('pointer', phase='up', gesture=gesture, x=435, y=434)
        device.tap_widget(name='rrAnnotationEdit')
        capture('webkit-annotation-adjust')
        frame = device.folder / 'artifacts/nickel-main.bmp'
        before = frame.read_bytes()
        # The captured title-page end handle is below the last glyph. Moving
        # it horizontally keeps the adjusted pointer on the same text line.
        gesture = 'end-handle'
        call('pointer', phase='down', gesture=gesture, x=967, y=542)
        call('pointer', phase='move', gesture=gesture, x=470, y=542)
        device.wait_until(lambda: frame.read_bytes() != before, timeout=4,
                          description='adjusted end handle painted')
        call('pointer', phase='up', gesture=gesture, x=470, y=542)
        call('tap', x=435, y=434)
        device.wait_widget(name='rrAnnotationSave')
        device.tap_widget(name='rrAnnotationSave')
        annotation_list('Type')
        capture('webkit-annotation-adjusted-list')
        close_marks()
        transcript.append({'action': 'adjust', 'backend': 'webkit', 'excerpt': 'Type'})

    def recover_renderer(original):
        container = 'virtualnickel-' + device.name
        boot = device.status().get('boot_id')
        assert boot
        def web_processes():
            rows = subprocess.check_output(['podman', 'top', container, 'pid,comm'],
                                           text=True).splitlines()[1:]
            return [int(pid) for pid, name in (row.split(maxsplit=1) for row in rows)
                    if name == 'WPEWebProcess']

        processes = web_processes()
        assert len(processes) == 1, processes
        pid = processes[0]
        before = cursor()
        # Inject a failure only into the content process of this disposable
        # reader. Leave Nickel and the supervisor to handle it normally.
        subprocess.run(['podman', 'exec', container, 'sh', '-ec',
                        'test "$(cat "/proc/$1/comm")" = WPEWebProcess && kill -TERM "$1"',
                        'sh', str(pid)], check=True, timeout=15)
        device.wait_until(lambda: pid not in web_processes(), timeout=15,
                          description='owned WebKit content process exited')
        forward = turn()
        assert forward['page'] == original['next_page'], (forward, original['next_page'])
        recovered = event('webkit-recovery-end', before)
        assert recovered['completed'] and recovered['page'] == forward['page'], recovered
        assert len([entry for entry in events() if entry.get('event') == 'webkit-recovery-start'
                    and entry.get('monotonic_ms', 0) > before]) == 1
        assert pid not in web_processes() and len(web_processes()) == 1
        capture('webkit-recovered-turn')
        returned = turn(-1)
        assert returned['page'] == original['page'], (returned, original['page'])
        capture('webkit-recovered-return')
        def bitmap(name):
            frame = (results / (name + '.bmp')).read_bytes()
            size = struct.unpack_from('<I', frame, 2)[0]
            assert frame[:2] == b'BM' and 54 <= size <= len(frame), name
            # The observer appends capture metadata after the declared BMP.
            return frame[:size]
        assert bitmap('webkit-recovered-return') == bitmap('webkit-open')
        assert device.status().get('boot_id') == boot
        transcript.append({'action': 'renderer-recovery', 'failed_content_pid': pid,
                           'result': recovered, 'return_frame_matches': True})

    def previews(backend, original):
        nonlocal current_page
        origin = original['page']
        assert origin == 1, 'The title page follows the cover in the pinned fixture'
        paper = device.wait_widget(name='rrPage')
        center = {'x': paper['x'] + paper['width'] // 2, 'y': paper['y'] + paper['height'] // 2}

        def previews_since(since):
            return [entry for entry in events() if entry.get('event') == 'page-preview-ready'
                    and entry.get('monotonic_ms', 0) > since]

        def wait_previews(since, pages):
            device.wait_until(lambda: pages <= {entry['page'] for entry in previews_since(since)},
                              timeout=60, description=backend + ' preview strip')

        call('tap', **center)
        device.wait_widget(name='rrNavSeek')
        capture(backend + '-preview-entry')
        before = cursor()
        device.tap_widget(name='rrNavSeek')
        device.wait_widget(name='rrBookPosition')
        wait_previews(before, {origin - 1, origin, origin + 1})
        capture(backend + '-preview-strip')
        assert not any(entry.get('event') == 'frame-ready' and entry.get('monotonic_ms', 0) > before
                       for entry in events()), 'Opening Navigate must not move the reading page'

        before = cursor()
        device.tap_widget(name='rrPreviewPreviousPage')
        device.wait_until(lambda: any(w['name'] == 'rrPreviewChapterCaption'
                          and w.get('text', '').startswith('PAGE 1 OF ') for w in device.ui()['widgets']),
                          description='the cover becomes the middle thumbnail')
        capture(backend + '-preview-cover')
        assert not previews_since(before), 'The cover and its neighbor should already be prepared'
        assert not any(entry.get('event') == 'frame-ready' and entry.get('monotonic_ms', 0) > before
                       for entry in events()), 'Previewing the cover must not move the reading page'
        device.tap_widget(name='rrPreviewNextPage')
        wait_previews(before, {origin + 1})

        before = cursor()
        device.tap_widget(name='rrPreviewNextPage')
        wait_previews(before, {origin + 2})
        capture(backend + '-preview-next')
        assert [entry['page'] for entry in previews_since(before)] == [origin + 2], previews_since(before)
        assert not any(entry.get('event') == 'frame-ready' and entry.get('monotonic_ms', 0) > before
                       for entry in events()), 'Thumbnail promotion must not turn the reading page'

        before = cursor()
        call('tap', **center)
        arrived = event('frame-ready', before)
        assert arrived['page'] == origin + 1, arrived
        capture(backend + '-preview-accepted')
        call('tap', **center)
        returning = device.wait_until(lambda: next((w for w in device.ui()['widgets']
                  if w['name'] in ('rrLinkReturn', 'rrScrubberReturn')), None),
                  description='return from accepted preview')
        before = cursor()
        device.tap_widget(name=returning['name'])
        returned = event('frame-ready', before)
        assert returned['page'] == origin, returned
        current_page = origin
        transcript.append({'action': 'previews', 'backend': backend, 'origin': origin,
                           'accepted': arrived['page'], 'returned': returned['page']})

    try:
        device.wait_log('reader: native controller ready', timeout=120)
        capture('library')
        first = open_book('webkit')
        capture('webkit-open')
        assert first['chapters'] > 0, first
        if previews_only:
            previews('webkit', first)
            close_book()
            launcher = device.onboard / '.adds/kobalt-webkit/bin/kobalt-webkit'
            hidden = launcher.with_suffix('.held')
            launcher.rename(hidden)
            try:
                comparison = open_book('crengine')
                comparison['page'] = contents('crengine')['page']
                previews('crengine', comparison)
                close_book()
            finally:
                hidden.rename(launcher)
            blocked = [entry for entry in events() if entry.get('event') == 'main-thread-blocked']
            assert not blocked, blocked
            report = {'status': 'passed', 'installed_mod': True, 'page_previews': True,
                      'overlap_reused': True, 'accept_and_return': True, 'helper_cleanup': True,
                      'main_thread_blocked': blocked, 'transcript': transcript}
            (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
            print('WEBKIT_INSTALLED_PREVIEWS_VERIFIED', flush=True)
            return
        if recovery_only:
            recover_renderer(first)
            close_book()
            blocked = [entry for entry in events() if entry.get('event') == 'main-thread-blocked']
            assert not blocked, blocked
            report = {'status': 'passed', 'installed_mod': True, 'renderer_recovery': True,
                      'same_nickel_boot': True, 'return_frame_matches': True, 'helper_cleanup': True,
                      'main_thread_blocked': blocked, 'transcript': transcript}
            (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
            print('WEBKIT_INSTALLED_RECOVERY_VERIFIED', flush=True)
            return
        if annotations_only:
            annotate('webkit')
            adjust_source_annotation()
            close_book()
            launcher = device.onboard / '.adds/kobalt-webkit/bin/kobalt-webkit'
            hidden = launcher.with_suffix('.held')
            launcher.rename(hidden)
            try:
                open_book('crengine')
                annotation_list(None)
                close_marks()
                contents('crengine')
                annotate('crengine')
                close_book()
            finally:
                hidden.rename(launcher)
            # Reopen from the persisted profile in a fresh Nickel process.
            # Keep the helper's license files outside the library scan.
            bundle = launcher.parent.parent
            held_bundle = device.folder / 'held-webkit'
            bundle.rename(held_bundle)
            try:
                device.restart(timeout=120)
                device.wait_library_ready(timeout=120)
            finally:
                held_bundle.rename(bundle)
            subprocess.run(['podman', 'update', '--cpus', '1', 'virtualnickel-' + device.name],
                           check=True, stdout=subprocess.DEVNULL)
            transcript.append({'action': 'restart', 'profile_reused': True})
            open_book('webkit')
            annotation_list('Type')
            capture('webkit-annotation-reopened')
            close_marks()
            close_book()
            blocked = [entry for entry in events() if entry.get('event') == 'main-thread-blocked']
            assert not blocked, blocked
            report = {'status': 'passed', 'installed_mod': True, 'source_highlights': True,
                      'hold_and_drag': True, 'adjusted_end_handle': True,
                      'backend_isolation': True, 'cold_reopen': True,
                      'helper_cleanup': True, 'main_thread_blocked': blocked, 'transcript': transcript}
            (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
            print('WEBKIT_INSTALLED_ANNOTATIONS_VERIFIED', flush=True)
            return
        if bookmarks_only:
            bookmark('webkit')
            saved_page = current_page
            close_book()
            launcher = device.onboard / '.adds/kobalt-webkit/bin/kobalt-webkit'
            hidden = launcher.with_suffix('.held')
            launcher.rename(hidden)
            try:
                value = open_book('crengine')
                assert value['bookmark_count'] == 0, value
                bookmark('crengine')
                close_book()
            finally:
                hidden.rename(launcher)
            value = open_book('webkit', expected_page=saved_page)
            assert value['bookmark_count'] == 1, value
            open_marks()
            device.wait_widget(name='rrBookmark0')
            capture('webkit-bookmark-reopened')
            close_marks()
            close_book()
            blocked = [entry for entry in events() if entry.get('event') == 'main-thread-blocked']
            assert not blocked, blocked
            report = {'status': 'passed', 'installed_mod': True, 'source_bookmarks': True,
                      'bookmark_return': True, 'backend_isolation': True, 'reopen': True,
                      'helper_cleanup': True, 'main_thread_blocked': blocked, 'transcript': transcript}
            (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
            print('WEBKIT_INSTALLED_BOOKMARKS_VERIFIED', flush=True)
            return
        if search_only:
            search('webkit')
            if rotate_after_search:
                contents('webkit')
                turn()
                turn(-1)
                rotations()
            saved_page = current_page
            close_book()
            launcher = device.onboard / '.adds/kobalt-webkit/bin/kobalt-webkit'
            hidden = launcher.with_suffix('.held')
            launcher.rename(hidden)
            try:
                open_book('crengine')
                search('crengine')
                close_book()
            finally:
                hidden.rename(launcher)
            open_book('webkit', expected_page=saved_page)
            memory_sample('after-reopen')
            close_book()
            blocked = [entry for entry in events() if entry.get('event') == 'main-thread-blocked']
            assert not blocked, blocked
            report = {'status': 'passed', 'installed_mod': True, 'source_search': True,
                      'search_return': True, 'creengine_comparison': True, 'reopen': True,
                      'helper_cleanup': True, 'main_thread_blocked': blocked, 'transcript': transcript}
            if rotate_after_search:
                report['rotation_after_search'] = True
                assert all('cgroup' in sample for sample in memory_samples), memory_samples
                peak = max(int(sample['cgroup']['peak']) for sample in memory_samples)
                limit = int(memory_samples[-1]['cgroup']['max'])
                # Leave room for the next frame/keyboard allocations rather
                # than passing only because the kernel reclaimed at its cap.
                assert peak + 32 * 1024 * 1024 < limit, (peak, limit)
                assert all(not sample['container'].get('OOMKilled') for sample in memory_samples)
                assert all(int(dict(line.split() for line in sample['cgroup']['events'].splitlines())
                               .get('oom_kill', '-1')) == 0 for sample in memory_samples)
                report.update(memory_peak_bytes=peak, memory_limit_bytes=limit,
                              memory_headroom_bytes=limit - peak, memory_oom_kills=0)
            (results / 'installed.json').write_text(json.dumps(report, indent=2) + '\n')
            print('WEBKIT_INSTALLED_SEARCH_VERIFIED', flush=True)
            return
        contents('webkit')
        forward = turn()
        assert forward['page'] > first['page']
        capture('webkit-next')
        backward = turn(-1)
        assert backward['page'] == first['page']
        # The installed UI and helper must survive both geometry and power
        # transitions. Report markers are independent of persistent positions.
        rotations()
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
        memory_sample('final')
        memory = subprocess.run(['podman', 'inspect', '--format', '{{json .State}}',
                                 'virtualnickel-' + device.name], capture_output=True, text=True)
        (results / 'container-state.json').write_text(memory.stdout)
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
