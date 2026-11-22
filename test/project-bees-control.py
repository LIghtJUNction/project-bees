"""Unprivileged bounded tests: never scan a host mount or require Btrfs."""
from pathlib import Path
import signal
import sqlite3
import subprocess
import tempfile
import time

binary = Path(__file__).resolve().parents[1] / 'bin/project-bees'
with tempfile.TemporaryDirectory() as tmp:
    state = Path(tmp) / 'state'
    root = Path(tmp) / 'root'
    root.mkdir()
    def control(action):
        return subprocess.check_output([str(binary), '--state-dir', str(state), '--' + action], text=True)
    def wait_for(fragment):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            value = control('status')
            if fragment in value:
                return value
            time.sleep(.1)
        raise AssertionError(value)
    subprocess.run([str(binary), '--state-dir', str(state), '--root', str(root), '--once'], check=True, capture_output=True, timeout=10)
    with sqlite3.connect(state / 'index.sqlite3') as seed:
        seed.execute("insert into files(id,path,pending) values(1,?,1)", (str(root / 'fixture'),))
        seed.execute("insert into chunks values(1,0,4096,123)")
    assert 'running=0 requested=1 paused=0' in control('pause')
    assert 'requested=1' in control('pause')
    with (Path(tmp) / 'log').open('w') as log:
        daemon = subprocess.Popen([str(binary), '--state-dir', str(state), '--root', str(root), '--interval', '1'], stdout=log, stderr=log)
        try:
            wait_for('running=1 requested=1 paused=1')
            db = sqlite3.connect(state / 'index.sqlite3')
            assert db.execute('pragma integrity_check').fetchone() == ('ok',)
            # Paused daemon sleeps; Linux process CPU ticks stay essentially fixed.
            def ticks():
                fields = Path(f'/proc/{daemon.pid}/stat').read_text().split()
                return int(fields[13]) + int(fields[14])
            assert db.execute('select hash from chunks').fetchall() == [(123,)]
            before = ticks()
            time.sleep(2.2)
            assert ticks() - before <= 5
            assert 'requested=0' in control('resume')
            wait_for('requested=0 paused=0')
            control('resume')
            wait_for('requested=0 paused=0')
            control('pause')
            wait_for('requested=1 paused=1')
            # Simulate a latched automatic request. Manual remains authoritative.
            (state / 'auto-pause').write_text('fixture')
            assert 'manual=1 automatic=1' in control('status')
            control('resume')
            wait_for('requested=0 paused=0')
            control('pause')
            wait_for('requested=1 paused=1')
            daemon.send_signal(signal.SIGTERM)
            assert daemon.wait(timeout=5) == 0
            assert 'running=0 requested=1 paused=0' in control('status')
            assert db.execute('pragma integrity_check').fetchone() == ('ok',)
            db.close()
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                daemon.wait(timeout=5)
    # A clean --once run can reopen the committed index after resume.
    control('resume')
    subprocess.run([str(binary), '--state-dir', str(state), '--root', str(root), '--once'], check=True, capture_output=True, timeout=10)
print('control integration passed')

# Production checkpoint loop, driven by a file instead of host PSI.
harness = binary.parent / 'project-bees-control-loop-test'
with tempfile.TemporaryDirectory() as tmp:
    state = Path(tmp) / 'state'
    psi = Path(tmp) / 'psi'
    def pressure(value):
        staging = psi.with_suffix('.new')
        staging.write_text(f'some avg10={value} avg60=0 avg300=0 total=1\n')
        staging.replace(psi)
    pressure(25)
    with (Path(tmp) / 'log').open('w') as log:
        daemon = subprocess.Popen([str(harness), str(state), str(psi)], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 16
            while time.monotonic() < deadline and not (state / 'paused').exists():
                time.sleep(.1)
            assert 'running=1 requested=1 paused=1 manual=0 automatic=1' in control('status')
            control('pause')
            psi.unlink() # Lost PSI must not release either request.
            time.sleep(2.2)
            assert 'requested=1 paused=1 manual=1 automatic=1' in control('status')
            pressure(90)
            control('resume')
            wait_for('requested=0 paused=0')
            time.sleep(3.2)
            assert 'requested=0 paused=0' in control('status')
            pressure(5)
            time.sleep(2.2)
            pressure(25)
            deadline = time.monotonic() + 16
            while time.monotonic() < deadline and not (state / 'paused').exists():
                time.sleep(.1)
            assert 'requested=1 paused=1 manual=0 automatic=1' in control('status')
            daemon.send_signal(signal.SIGTERM)
            assert daemon.wait(timeout=5) == 0
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                daemon.wait(timeout=5)
print('automatic PSI loop passed')

with tempfile.TemporaryDirectory() as tmp:
    state = Path(tmp) / 'state'
    missing = Path(tmp) / 'missing-psi'
    with (Path(tmp) / 'log').open('w') as log:
        daemon = subprocess.Popen([str(harness), str(state), str(missing)], stdout=log, stderr=log)
        try:
            wait_for('running=1 requested=0 paused=0')
            time.sleep(2.2)
            assert 'requested=0 paused=0' in control('status')
            control('pause')
            wait_for('requested=1 paused=1')
            control('resume')
            wait_for('requested=0 paused=0')
        finally:
            daemon.send_signal(signal.SIGTERM)
            assert daemon.wait(timeout=5) == 0
print('missing PSI manual fallback passed')
