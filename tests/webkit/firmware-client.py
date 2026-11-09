"""Run inside the existing offline firmware container; no application build logic."""
from pathlib import Path
import hashlib
import json
import os
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import time

bundle, destination = map(Path, sys.argv[1:3])
selected_mode = sys.argv[3] if len(sys.argv) > 3 else 'experiment'
assert selected_mode in ('experiment', 'device-unsandboxed')
destination.mkdir(parents=True, exist_ok=True)
records = []


def descendants(root):
    table = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = path.read_text().rsplit(')', 1)[1].split()
            table[int(path.parent.name)] = int(fields[1])
        except (OSError, ValueError, IndexError):
            pass
    selected = {root}
    while True:
        more = {pid for pid, parent in table.items() if parent in selected}
        if more <= selected:
            return selected - {root}
        selected |= more


class Client:
    def __init__(self, name, mode=selected_mode):
        self.name = name
        self.mode = mode
        self.ready = 'KWRP/1 0 READY ' + ('sandbox-required' if mode == 'required' else mode)
        self.scratch_before = set(Path('/tmp').glob('kobalt-webkit-*'))
        self.log = (destination / (name + '.stderr')).open('w')
        command = ['/usr/bin/vn-offline', '/usr/bin/qemu-arm', '-cpu', 'cortex-a7',
                   '-L', '/firmware/rootfs', '-U', 'LD_PRELOAD', str(bundle / 'bin/kobalt-webkit'),
                   '--qemu-prefix', '/firmware/rootfs']
        if mode == 'experiment':
            command.append('--allow-unsandboxed-experiment')
        elif mode == 'device-unsandboxed':
            command.append('--allow-unsandboxed-device')
        else:
            assert mode == 'required'
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            env=dict(os.environ, WEBKIT_EXEC_PATH='/poison', DISPLAY='poison',
                     WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS='1',
                     KOBALT_WEBKIT_SANDBOX_MODE='device-unsandboxed',
                     KOBALT_WEBKIT_EXPERIMENT='1'),
            start_new_session=True,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = b''
        self.lines = []
        self.tracked = set()
        self.runtime = None

    def receive(self, timeout=75):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.pending:
            if not self.selector.select(max(0, deadline - time.monotonic())):
                raise TimeoutError(self.name + ': protocol deadline')
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError(self.name + ': renderer closed output')
            self.pending += chunk
        line, self.pending = self.pending.split(b'\n', 1)
        text = line.decode('ascii')
        self.lines.append(text)
        assert text.startswith('KWRP/1 '), text
        return text

    def send(self, commands):
        self.process.stdin.write(commands.encode('ascii'))
        self.process.stdin.flush()

    def render(self):
        assert self.receive() == self.ready
        self.send('KWRP/1 1 PING\nKWRP/1 2 RENDER 800 1100\n'
                  'KWRP/1 3 PING\nKWRP/1 4 RENDER 800 1100\n')
        answers = [self.receive() for _ in range(4)]
        assert 'KWRP/1 1 OK PONG' in answers
        assert 'KWRP/1 3 OK PONG' in answers
        assert 'KWRP/1 4 ERROR busy' in answers
        snapshot = next(line for line in answers if line.startswith('KWRP/1 2 OK SNAPSHOT 800 1100 '))
        assert answers.index('KWRP/1 3 OK PONG') < answers.index(snapshot), answers
        path = Path(snapshot.split()[-1])
        assert path.is_file() and path.name == 'snapshot.png'
        self.runtime = path.parent
        image = path.read_bytes()
        assert image[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', image[16:24]) == (800, 1100)
        shutil.copy2(path, destination / (self.name + '.png'))
        self.tracked |= descendants(self.process.pid)
        for pid in self.tracked:
            try:
                modules = {Path(line.split()[-1]).name for line in Path('/proc', str(pid), 'maps').read_text().splitlines() if '/' in line}
                unexpected = [name for name in modules if name.startswith(('libEGL', 'libGLES', 'libGL.', 'libGLX', 'libvulkan', 'libgbm', 'libdrm', 'libgst'))]
                assert not unexpected, (pid, unexpected)
            except FileNotFoundError:
                pass
        return hashlib.sha256(image).hexdigest()

    def kill_named(self, name):
        selected = []
        for pid in descendants(self.process.pid):
            try:
                arguments = Path('/proc', str(pid), 'cmdline').read_bytes().split(b'\0')
                if any(Path(arg.decode(errors='replace')).name == name for arg in arguments if arg):
                    selected.append(pid)
            except OSError:
                pass
        assert len(selected) == 1, (name, selected)
        self.tracked |= descendants(self.process.pid)
        os.kill(selected[0], signal.SIGKILL)

    def close(self, expected):
        self.tracked |= descendants(self.process.pid)
        self.process.stdin.close()
        result = self.process.wait(timeout=10)
        assert result == expected, (self.name, result, self.log.name)
        assert not any(Path('/proc', str(pid)).exists() for pid in self.tracked), self.tracked
        assert self.runtime is None or not self.runtime.exists(), self.runtime
        scratch_after = set(Path('/tmp').glob('kobalt-webkit-*'))
        assert scratch_after <= self.scratch_before, scratch_after - self.scratch_before
        self.selector.close()
        self.process.stdout.close()
        self.log.close()
        assert 'CRITICAL' not in Path(self.log.name).read_text(), self.log.name
        (destination / (self.name + '.protocol')).write_text('\n'.join(self.lines) + '\n')
        records.append(dict(case=self.name, mode=self.mode, result=result, cleanup=True))

    def abort(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.log.close()


def main():
    active = None
    try:
        checksums = []
        for label in ('cold', 'repeat', 'web-crash', 'worker-crash', 'after-crash'):
            active = Client(label)
            checksums.append(active.render())
            if label == 'web-crash':
                active.kill_named('WPEWebProcess')
                assert active.receive() == 'KWRP/1 0 ERROR web-process-terminated'
                active.close(1)
            elif label == 'worker-crash':
                active.kill_named('kobalt-webkit-worker')
                active.close(137)
            else:
                active.send('KWRP/1 5 RENDER 600 800\n')
                resize = active.receive()
                assert resize.startswith('KWRP/1 5 OK SNAPSHOT 600 800 '), resize
                active.send('KWRP/1 6 SHUTDOWN\n')
                assert active.receive() == 'KWRP/1 6 OK BYE'
                active.close(0)
            active = None
        assert len(set(checksums)) == 1, checksums
        for label, request, response in (
            ('version', 'KWRP/2 1 PING\n', 'KWRP/1 0 ERROR invalid-request'),
            ('oversized', 'X' * 8193 + '\n', 'KWRP/1 0 ERROR oversized-request'),
            ('duplicate', 'KWRP/1 1 PING\nKWRP/1 1 PING\n', 'KWRP/1 0 ERROR invalid-request'),
        ):
            active = Client(label)
            assert active.receive() == active.ready
            active.send(request)
            if label == 'duplicate':
                assert active.receive() == 'KWRP/1 1 OK PONG'
            assert active.receive() == response
            active.close(2)
            active = None
        active = Client('eof')
        assert active.receive() == active.ready
        active.send('KWRP/1 1 RENDER 800 1100\n')
        active.tracked |= descendants(active.process.pid)
        active.close(0)
        active = Client('backpressure')
        assert active.receive() == active.ready
        os.set_blocking(active.process.stdin.fileno(), False)
        payload = ''.join('KWRP/1 ' + str(index) + ' PING\n' for index in range(1, 10001)).encode('ascii')
        offset = 0
        deadline = time.monotonic() + 15
        while active.process.poll() is None and time.monotonic() < deadline:
            try:
                if offset < len(payload):
                    offset += os.write(active.process.stdin.fileno(), payload[offset:])
            except BlockingIOError:
                pass
            except BrokenPipeError:
                break
            time.sleep(.01)
        active.close(1)
        active = None
        active = Client('sandbox-required', mode='required')
        try:
            assert active.receive() == 'KWRP/1 0 READY sandbox-required'
            active.send('KWRP/1 1 RENDER 800 1100\n')
            while True:
                response = active.receive()
                assert 'OK SNAPSHOT' not in response, 'Unsandboxed normal-mode render was accepted'
        except EOFError:
            pass
        result = active.process.wait(timeout=10)
        assert result != 0, 'Required sandbox failure was accepted'
        assert 'Required WebKit sandbox unavailable' in Path(active.log.name).read_text()
        active.close(result)
        active = None
        other_mode = 'experiment' if selected_mode == 'device-unsandboxed' else 'device-unsandboxed'
        active = Client('other-explicit-mode', mode=other_mode)
        assert active.render() == checksums[0]
        active.send('KWRP/1 5 SHUTDOWN\n')
        assert active.receive() == 'KWRP/1 5 OK BYE'
        active.close(0)
        active = None
        for label, mode, override in (
            ('missing', None, '1'), ('unknown', 'unknown', '1'),
            ('required-disabled', 'required', '1'),
            ('device-missing-override', 'device-unsandboxed', None),
            ('experiment-enabled', 'experiment', '0'),
        ):
            environment = dict(os.environ, KOBALT_WEBKIT_BUNDLE=str(bundle), KOBALT_WEBKIT_RUNTIME='/tmp')
            for key, value in (('KOBALT_WEBKIT_SANDBOX_MODE', mode),
                               ('WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS', override)):
                environment.pop(key, None)
                if value is not None:
                    environment[key] = value
            libraries = str(bundle / 'private') + ':/firmware/rootfs/usr/lib:/firmware/rootfs/lib'
            result = subprocess.run(
                ['/usr/bin/vn-offline', '/usr/bin/qemu-arm', '-cpu', 'cortex-a7',
                 '-L', '/firmware/rootfs', '-U', 'LD_PRELOAD', '-E', 'LD_LIBRARY_PATH=' + libraries,
                 str(bundle / 'bin/kobalt-webkit-worker')],
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            (destination / ('worker-mode-' + label + '.stderr')).write_bytes(result.stderr)
            assert result.returncode == 2 and not result.stdout, (label, result.returncode, result.stdout)
            records.append(dict(case='worker-mode-' + label, result=2, cleanup=True))
        (destination / 'results.json').write_text(json.dumps(dict(primary_mode=selected_mode, runs=records, snapshot_sha256=checksums[0]), indent=2))
        print('WEBKIT_FIRMWARE_OK ' + json.dumps(records), flush=True)
    finally:
        if active is not None:
            active.abort()


if __name__ == "__main__":
    main()
