"""Exercise the scoped daemon and verify physical sharing with FIEMAP."""

import fcntl
import hashlib
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import time


BINARY = Path(__file__).resolve().parents[1] / "bin" / "project-bees"
FIEMAP = 0xC020660B
FIEMAP_EXTENT_SHARED = 0x2000


def git(*args):
    subprocess.run(["git", *args], check=True, capture_output=True)


def put(path, content):
    with path.open("wb") as output:
        output.write(content)
        output.flush()
        os.fsync(output.fileno())
    return path


def extents(path):
    header = struct.Struct("=QQIIII")
    extent = struct.Struct("=5Q4I")
    count = 128
    buf = bytearray(header.size + count * extent.size)
    header.pack_into(buf, 0, 0, 2**64 - 1, 1, 0, count, 0)
    with path.open("rb") as stream:
        fcntl.ioctl(stream, FIEMAP, buf, True)
    mapped = header.unpack_from(buf)[3]
    return [extent.unpack_from(buf, header.size + extent.size * i) for i in range(mapped)]


def shared(source, dest):
    def layout(path):
        return [(logical, physical, length) for logical, physical, length, *rest in extents(path)
                if rest[2] & FIEMAP_EXTENT_SHARED]

    return bool(layout(source)) and layout(source) == layout(dest)


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def wait_for(predicate, process, log):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        if predicate():
            return
        assert process.poll() is None, log.read_text()
        time.sleep(0.1)
    raise AssertionError(log.read_text())


def test(root):
    scan = root / "scan"
    scan.mkdir()
    a = scan / "a"
    nested = a / "nested"
    bare = scan / "bare.git"
    worktree = scan / "worktree"
    fake = scan / "fake"
    git("init", "-q", a)
    git("init", "-q", nested)
    git("init", "-q", "--bare", bare)
    git("-C", a, "-c", "user.name=Test", "-c", "user.email=test@example.com",
        "commit", "--allow-empty", "-qm", "initial")
    git("-C", a, "worktree", "add", "-q", "-b", "test-worktree", worktree)
    (fake / ".git").mkdir(parents=True)
    (scan / "symlink-repo").symlink_to(a)
    (a / "symlink-external").symlink_to(fake)
    args = [BINARY, "--root", scan, "--state-dir", root / "state"]
    discovered = subprocess.check_output([*args, "--discover"], text=True).splitlines()
    assert set(discovered) == {str(a), str(nested), str(bare), str(worktree)}, discovered

    common = os.urandom(512 * 1024 + 22133)
    inside = os.urandom(256 * 1024)
    a_file = put(a / "common", common)
    b_file = put(nested / "common", common)
    b_file.chmod(0o444)
    metadata = put(a / ".git" / "test-object", common)
    external = put(fake / "outside", common)
    solo1 = put(a / "solo1", inside)
    solo2 = put(a / "solo2", inside)
    linked = put(fake / "outside-hardlink", common)
    os.link(linked, worktree / "hardlink")
    assert not shared(a_file, b_file)
    before = {path: digest(path) for path in (a_file, b_file, metadata, external, solo1, solo2, linked)}
    result = subprocess.check_output([*args, "--once"], text=True)
    assert "scan repositories=4" in result, result
    assert shared(a_file, b_file) and shared(metadata, b_file), result
    assert not shared(a_file, external) and not shared(a_file, linked)
    assert not shared(solo1, solo2)
    assert before == {path: digest(path) for path in before}
    assert b_file.stat().st_mode & 0o777 == 0o444

    log = root / "daemon.log"
    with log.open("w") as output, subprocess.Popen(
        [*args, "--interval", "1"], stdout=output, stderr=subprocess.STDOUT
    ) as daemon:
        try:
            wait_for(lambda: "hashed_files=0" in log.read_text(), daemon, log)
            newcomer = scan / "new"
            git("init", "-q", newcomer)
            new_file = put(newcomer / "duplicate", common)
            wait_for(lambda: shared(a_file, new_file) or shared(b_file, new_file), daemon, log)
            update = put(worktree / "changed", os.urandom(len(inside)))

            def indexed():
                with sqlite3.connect(root / "state" / "index.sqlite3", timeout=2) as db:
                    return db.execute("SELECT 1 FROM files WHERE path=? AND pending=0",
                                      (os.fsencode(update),)).fetchone() is not None

            wait_for(indexed, daemon, log)
            before_update = update.stat()
            put(update, inside)
            os.utime(update, ns=(before_update.st_atime_ns, before_update.st_mtime_ns))
            wait_for(lambda: shared(solo1, update) and shared(solo2, update), daemon, log)
            assert digest(update) == digest(solo1)
        finally:
            daemon.terminate()
            assert daemon.wait(timeout=10) == 0, log.read_text()

    print("Passed Git discovery, cross-repository physical sharing, scope, content, and daemon updates")


if __name__ == "__main__":
    test_parent = Path(os.environ.get("PROJECT_BEES_TEST_DIR", BINARY.parents[1])).resolve()
    fstype = subprocess.check_output(["stat", "-f", "-c", "%T", test_parent], text=True).strip()
    if fstype != "btrfs":
        raise SystemExit("Set PROJECT_BEES_TEST_DIR to a writable directory on Btrfs")
    with tempfile.TemporaryDirectory(prefix="project-bees-test-", dir=test_parent) as directory:
        test(Path(directory))
