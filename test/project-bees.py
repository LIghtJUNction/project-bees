"""Exercise the scoped daemon and verify physical sharing with FIEMAP."""

import fcntl
import hashlib
import json
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


def check_activity(report, phase=None):
    assert report["version"] == 1, report
    activity = report["activity"]
    assert activity["phase"] in {"scanning", "deduplicating", "idle", "stopped"}, activity
    if phase is not None:
        assert activity["phase"] == phase, activity
    for field in ("phaseStartedAt", "repositories", "scannedFiles", "hashedFiles",
                  "indexedChunks", "processedFiles", "matchedRanges", "rejectedRanges", "nextScanAt"):
        assert type(activity[field]) is int and activity[field] >= 0, activity
    assert 0 < activity["phaseStartedAt"] <= report["updatedAt"], activity
    assert activity["hashedFiles"] <= activity["scannedFiles"], activity
    if activity["phase"] == "idle":
        assert activity["nextScanAt"] > activity["phaseStartedAt"], activity
    else:
        assert activity["nextScanAt"] == 0, activity
    return activity


def test(root):
    scan = root / "scan"
    scan.mkdir()
    a = scan / "a"
    nested = a / "nested"
    bare = scan / "bare.git"
    worktree = scan / "worktree"
    fake = scan / "fake"
    template = root / "empty-template"
    template.mkdir()
    git("init", "-q", f"--template={template}", a)
    git("init", "-q", f"--template={template}", nested)
    git("init", "-q", "--bare", f"--template={template}", bare)
    git("-C", a, "-c", "user.name=Test", "-c", "user.email=test@example.com",
        "commit", "--allow-empty", "-qm", "initial")
    git("-C", a, "worktree", "add", "-q", "-b", "test-worktree", worktree)
    (fake / ".git").mkdir(parents=True)
    (scan / "symlink-repo").symlink_to(a)
    (a / "symlink-external").symlink_to(fake)
    args = [BINARY, "--root", scan, "--state-dir", root / "state"]
    stats_args = [BINARY, "--state-dir", root / "state", "--stats"]

    def stats(expected):
        report = json.loads(subprocess.check_output(stats_args))
        check_activity(report, "stopped")
        assert report["totalBytes"] == expected, report
        assert sum(proj["bytes"] for proj in report["projects"]) == expected
        assert sum(proj["sourceBytes"] for proj in report["projects"]) == expected
        assert sum(pair["bytes"] for pair in report["pairs"]) == expected
        return report

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
    report = stats(2 * len(common))
    assert {proj["path"] for proj in report["projects"]} == set(discovered)
    assert len(report["pairs"]) == 1
    assert {report["pairs"][0]["a"], report["pairs"][0]["b"]} == {str(a), str(nested)}
    activity = report["activity"]
    assert activity["repositories"] == 4, activity
    assert activity["scannedFiles"] == activity["hashedFiles"] >= 5, activity
    assert activity["processedFiles"] == activity["hashedFiles"], activity
    assert activity["indexedChunks"] >= 19, activity
    assert activity["matchedRanges"] >= report["rangeCount"] > 0, activity
    # Force a rescan of an already shared file and keep the persisted total.
    os.utime(a_file, None)
    subprocess.run([*args, "--once"], check=True, capture_output=True)
    repeated = stats(2 * len(common))
    assert repeated["startedAt"] == report["startedAt"]
    assert repeated["activity"]["hashedFiles"] == 1, repeated["activity"]
    assert repeated["activity"]["indexedChunks"] == 5, repeated["activity"]

    log = root / "daemon.log"
    with log.open("w") as output, subprocess.Popen(
        [*args, "--interval", "1"], stdout=output, stderr=subprocess.STDOUT
    ) as daemon:
        try:
            wait_for(lambda: "hashed_files=0" in log.read_text(), daemon, log)

            def idle():
                current = json.loads((root / "state" / "stats.json").read_text())
                activity = check_activity(current)
                if activity["phase"] != "idle":
                    return False
                assert activity["nextScanAt"] == activity["phaseStartedAt"] + 1, activity
                assert activity["hashedFiles"] == activity["indexedChunks"] == 0, activity
                return True

            wait_for(idle, daemon, log)
            newcomer = scan / "new"
            git("init", "-q", f"--template={template}", newcomer)
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

    stats(3 * len(common) + 2 * len(inside))
    print("Passed Git discovery, physical sharing, scope, live updates, persistent statistics, and activity states")


def test_stats_encoding(root):
    scan = root / "encoding"
    scan.mkdir()
    state = root / "encoding-state"
    template = root / "empty-template"
    names = [
        ("unicode-¢-仓库-🐝".encode(), "unicode-¢-仓库-🐝"),
        (b'escaped-"-\\-\n-\t', 'escaped-"-\\-\n-\t'),
        (b"invalid-\xff\x80", "invalid-\ufffd\ufffd"),
        (b"overlong-\xc0\xaf\xe0\x80\xaf", "overlong-" + "\ufffd" * 5),
        (b"surrogate-\xed\xa0\x80", "surrogate-" + "\ufffd" * 3),
        (b"out-of-range-\xf4\x90\x80\x80", "out-of-range-" + "\ufffd" * 4),
        (b"truncated-\xe2\x82", "truncated-\ufffd\ufffd"),
        (b"broken-\xe2(\xa1", "broken-\ufffd(\ufffd"),
    ]
    content = os.urandom(32 * 1024)
    expected_paths = {str(scan) + "/" + display for _, display in names}
    raw_paths = set()
    for raw, _ in names:
        path = os.fsencode(scan) + b"/" + raw
        raw_paths.add(path)
        repo = Path(os.fsdecode(path))
        git("init", "-q", f"--template={template}", repo)
        put(repo / "common", content)

    args = [BINARY, "--root", scan, "--state-dir", state, "--once"]
    subprocess.run(args, check=True, capture_output=True)
    raw_report = subprocess.check_output([BINARY, "--state-dir", state, "--stats"])
    assert raw_report == (state / "stats.json").read_bytes()
    report = json.loads(raw_report.decode("utf-8"))
    activity = check_activity(report, "stopped")
    assert activity["repositories"] == len(names), activity
    assert activity["scannedFiles"] == activity["hashedFiles"] == len(names), activity
    assert activity["indexedChunks"] == activity["processedFiles"] == len(names), activity
    assert {row["path"] for row in report["projects"]} == expected_paths, report
    assert all(row["a"] in expected_paths and row["b"] in expected_paths for row in report["pairs"])
    assert report["totalBytes"] == (len(names) - 1) * len(content), report
    assert sum(row["bytes"] for row in report["projects"]) == report["totalBytes"]
    assert sum(row["bytes"] for row in report["pairs"]) == report["totalBytes"]
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == raw_paths
    subprocess.run(args, check=True, capture_output=True)
    repeated = json.loads((state / "stats.json").read_text(encoding="utf-8"))
    activity = check_activity(repeated, "stopped")
    assert activity["scannedFiles"] == len(names), activity
    assert activity["hashedFiles"] == activity["indexedChunks"] == activity["processedFiles"] == 0, activity
    assert activity["matchedRanges"] == activity["rejectedRanges"] == 0, activity
    assert repeated["totalBytes"] == report["totalBytes"]
    print("Passed UTF-8 statistics, JSON escaping, exact path keys, and repeat-scan accounting")


if __name__ == "__main__":
    test_parent = Path(os.environ.get("PROJECT_BEES_TEST_DIR", BINARY.parents[1])).resolve()
    fstype = subprocess.check_output(["stat", "-f", "-c", "%T", test_parent], text=True).strip()
    if fstype != "btrfs":
        raise SystemExit("Set PROJECT_BEES_TEST_DIR to a writable directory on Btrfs")
    with tempfile.TemporaryDirectory(prefix="project-bees-test-", dir=test_parent) as directory:
        test(Path(directory))
        test_stats_encoding(Path(directory))
