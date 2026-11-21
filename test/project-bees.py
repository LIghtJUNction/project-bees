"""Exercise the scoped daemon and verify physical sharing with FIEMAP."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import time


BINARY = Path(__file__).resolve().parents[1] / "bin" / "project-bees"
CHUNK_SIZE = 128 * 1024
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


def shared_range(source, source_offset, dest, dest_offset, length):
    def layout(path, offset):
        result = []
        covered = offset
        for extent in extents(path):
            logical, physical, size = extent[:3]
            begin = max(offset, logical)
            end = min(offset + length, logical + size)
            if begin >= end:
                continue
            if begin != covered or not physical or not extent[5] & FIEMAP_EXTENT_SHARED:
                return None
            address = physical + begin - logical
            if result and result[-1][0] + result[-1][1] == address:
                result[-1] = (result[-1][0], result[-1][1] + end - begin)
            else:
                result.append((address, end - begin))
            covered = end
        return result if covered == offset + length else None

    source_layout = layout(source, source_offset)
    return source_layout is not None and source_layout == layout(dest, dest_offset)


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
    assert activity["scanKind"] in {"full", "priority"}, activity
    assert type(activity["scanRoot"]) is str, activity
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
    events = report["recentEvents"]
    assert type(events) is list and len(events) <= 32, events
    for event in events:
        assert type(event) is dict, event
        assert type(event["at"]) is int and 0 < event["at"] <= report["updatedAt"], event
        assert type(event["bytes"]) is int and event["bytes"] > 0, event
        assert type(event["source"]) is str and event["source"], event
        assert type(event["destination"]) is str and event["destination"], event
    assert [event["at"] for event in events] == sorted(event["at"] for event in events), events
    assert sum(event["bytes"] for event in events) <= report["totalBytes"], report
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
    events = report["recentEvents"]
    expected_files = {path + "/common" for path in expected_paths}
    assert len(events) == len(names) - 1, events
    assert {event[field] for event in events for field in ("source", "destination")} == expected_files, events
    assert all(event["bytes"] == len(content) for event in events), events
    assert sum(event["bytes"] for event in events) == report["totalBytes"], report
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == raw_paths
    subprocess.run(args, check=True, capture_output=True)
    repeated = json.loads((state / "stats.json").read_text(encoding="utf-8"))
    activity = check_activity(repeated, "stopped")
    assert activity["scannedFiles"] == len(names), activity
    assert activity["hashedFiles"] == activity["indexedChunks"] == activity["processedFiles"] == 0, activity
    assert activity["matchedRanges"] == activity["rejectedRanges"] == 0, activity
    assert repeated["totalBytes"] == report["totalBytes"]
    assert repeated["recentEvents"] == [], repeated
    print("Passed UTF-8 statistics, JSON escaping, exact path keys, and repeat-scan accounting")


def make_repositories(root, name, names=("a", "b")):
    scan = root / name
    scan.mkdir()
    template = root / "empty-template"
    template.mkdir(exist_ok=True)
    repositories = [scan / name for name in names]
    for repository in repositories:
        git("init", "-q", f"--template={template}", repository)
    return scan, root / (name + "-state"), repositories


def run_once(scan, state, expected=None, *, env=None, priority_roots=(), output=None):
    command = [BINARY, "--root", scan, "--state-dir", state, "--once"]
    for priority in priority_roots:
        command.extend(["--priority-root", priority])
    process = subprocess.run(
        command, text=True, capture_output=True, env=env,
    )
    assert process.returncode == 0, process.stdout + process.stderr
    if output is not None:
        output.extend(process.stdout.splitlines())
    report = json.loads(subprocess.check_output([BINARY, "--state-dir", state, "--stats"], env=env))
    check_activity(report, "stopped")
    total = report["totalBytes"]
    if expected is not None:
        assert total == expected, (report, process.stdout)
    assert sum(project["bytes"] for project in report["projects"]) == total
    assert sum(project["sourceBytes"] for project in report["projects"]) == total
    assert sum(pair["bytes"] for pair in report["pairs"]) == total
    return report


def discover(scan, state, *, env=None, priority_roots=()):
    command = [BINARY, "--root", scan, "--state-dir", state, "--discover"]
    for priority in priority_roots:
        command.extend(["--priority-root", priority])
    return subprocess.check_output(command, text=True, env=env).splitlines()


def test_coalesced_ranges(root):
    scan, state, (a, b) = make_repositories(root, "coalesced")
    content = os.urandom(2 * 1024 * 1024 + 22133)
    source = put(a / "common", content)
    dest = put(b / "common", content)
    report = run_once(scan, state, len(content))
    assert source.read_bytes() == dest.read_bytes() == content
    assert shared_range(source, 0, dest, 0, len(content)), report
    assert report["activity"]["matchedRanges"] == 6, report
    assert report["activity"]["rejectedRanges"] == 0, report
    assert report["rangeCount"] == 3, report
    events = report["recentEvents"]
    assert len(events) == 1, events
    assert {events[0]["source"], events[0]["destination"]} == {str(source), str(dest)}, events
    assert events[0]["bytes"] == len(content) == report["totalBytes"], events
    os.utime(source, None)
    lines = []
    repeated = run_once(scan, state, len(content), output=lines)
    assert repeated["rangeCount"] == report["rangeCount"], repeated
    assert repeated["startedAt"] == report["startedAt"], repeated
    assert repeated["activity"]["hashedFiles"] == 1, repeated
    assert repeated["recentEvents"] == [], repeated
    assert any(line.startswith("dedup ranges=") and "newly_shared_bytes=0 " in line for line in lines), lines
    assert source.read_bytes() == dest.read_bytes() == content
    print("Passed bounded range coalescing, tail accounting, and shared-range rescanning")


def test_shifted_and_sparse_ranges(root):
    scan, state, (a, b) = make_repositories(root, "shifted")
    p, x, y, z, q = (os.urandom(CHUNK_SIZE) for _ in range(5))
    source_content = p + x + y + z
    dest_content = x + y + z + q
    source = put(a / "shifted", source_content)
    dest = put(b / "shifted", dest_content)
    report = run_once(scan, state, 3 * CHUNK_SIZE)
    assert source.read_bytes() == source_content
    assert dest.read_bytes() == dest_content
    assert shared_range(source, CHUNK_SIZE, dest, 0, 3 * CHUNK_SIZE), report
    assert report["activity"]["matchedRanges"] == 2, report
    assert report["activity"]["rejectedRanges"] == 0, report
    assert report["rangeCount"] == 1, report

    scan, state, (a, b) = make_repositories(root, "sparse-ranges")
    p, x, y, z, q, r = (os.urandom(CHUNK_SIZE) for _ in range(6))
    source = a / "sparse"
    with source.open("wb") as output:
        output.write(x)
        output.seek(2 * CHUNK_SIZE)
        output.write(y + z)
        output.flush()
        os.fsync(output.fileno())
    source_content = x + bytes(CHUNK_SIZE) + y + z
    dest_content = p + x + q + y + z + r
    dest = put(b / "gapped", dest_content)
    report = run_once(scan, state, 3 * CHUNK_SIZE)
    assert source.read_bytes() == source_content
    assert dest.read_bytes() == dest_content
    assert shared_range(source, 0, dest, CHUNK_SIZE, CHUNK_SIZE), report
    assert shared_range(source, 2 * CHUNK_SIZE, dest, 3 * CHUNK_SIZE, 2 * CHUNK_SIZE), report
    assert not shared_range(source, CHUNK_SIZE, dest, 2 * CHUNK_SIZE, CHUNK_SIZE), report
    assert report["activity"]["matchedRanges"] == 4, report
    # Crossing the source hole would compare zeroes with q and be rejected.
    assert report["activity"]["rejectedRanges"] == 0, report
    assert report["rangeCount"] == 2, report
    print("Passed shifted ranges, sparse gaps, and preservation of unmatched content")


def test_coalesced_hash_collision(root):
    scan, state, (a, b) = make_repositories(root, "hash-collision")
    x, y, q, z = (os.urandom(CHUNK_SIZE) for _ in range(4))
    source = put(a / "source", x + y)
    dest = put(b / "dest", q + z)
    run_once(scan, state, 0)
    source_stamp = source.stat()
    with sqlite3.connect(state / "index.sqlite3") as db:
        target_hash = db.execute(
            "SELECT c.hash FROM chunks c JOIN files f ON f.id=c.file WHERE f.path=? AND c.start=?",
            (os.fsencode(dest), CHUNK_SIZE),
        ).fetchone()[0]
        changed = db.execute(
            "UPDATE chunks SET hash=? WHERE file=(SELECT id FROM files WHERE path=?) AND start=?",
            (target_hash, os.fsencode(source), CHUNK_SIZE),
        ).rowcount
        assert changed == 1
    put(dest, x + z)
    assert (source.stat().st_mtime_ns, source.stat().st_ctime_ns) == (
        source_stamp.st_mtime_ns, source_stamp.st_ctime_ns,
    )
    report = run_once(scan, state, CHUNK_SIZE)
    assert source.read_bytes() == x + y
    assert dest.read_bytes() == x + z
    assert shared_range(source, 0, dest, 0, CHUNK_SIZE), report
    assert not shared_range(source, CHUNK_SIZE, dest, CHUNK_SIZE, CHUNK_SIZE), report
    assert report["activity"]["hashedFiles"] == 1, report
    assert report["activity"]["rejectedRanges"] > 0, report
    assert report["rangeCount"] == 1, report
    print("Passed collision rejection and single-chunk fallback after a failed merged range")


def test_nontransitive_pending(root):
    scan, state, (a, b) = make_repositories(root, "pending-snapshot")
    x, y = (os.urandom(CHUNK_SIZE) for _ in range(2))
    a_file = put(a / "xy", x + y)
    b_file = put(b / "y", y)
    run_once(scan, state, CHUNK_SIZE)
    assert shared_range(a_file, CHUNK_SIZE, b_file, 0, CHUNK_SIZE)
    c = scan / "c"
    git("init", "-q", f"--template={root / 'empty-template'}", c)
    c_file = put(c / "x", x)
    report = run_once(scan, state, 2 * CHUNK_SIZE)
    assert report["activity"]["hashedFiles"] == 1, report
    assert report["activity"]["processedFiles"] == 2, report
    assert shared_range(a_file, 0, c_file, 0, CHUNK_SIZE), report
    assert shared_range(a_file, CHUNK_SIZE, b_file, 0, CHUNK_SIZE), report
    assert a_file.read_bytes() == x + y
    assert b_file.read_bytes() == y
    assert c_file.read_bytes() == x
    print("Passed fixed fresh-file snapshot without transitive pending expansion")


def test_repeated_block_convergence(root):
    scan, state, (a, b) = make_repositories(root, "repeated-block-convergence")
    block = os.urandom(CHUNK_SIZE)
    content = block * 16
    source = put(a / "repeated", content)
    dest = put(b / "repeated", content)
    report = run_once(scan, state)
    for path in (source, dest):
        assert path.read_bytes() == content
        for index in range(16):
            assert shared_range(source, 0, path, index * CHUNK_SIZE, CHUNK_SIZE), (path, index, report)
    print("Passed convergence of repeated blocks to one shared physical range")


def test_cross_file_source_convergence(root):
    scan, state, (a, b) = make_repositories(root, "cross-file-source-convergence")
    x, y, z = (os.urandom(CHUNK_SIZE) for _ in range(3))
    contents = {
        a / "a1": x + y,
        a / "a2": z + y,
        b / "b1": z + y,
        b / "b2": x + y,
    }
    for path, content in contents.items():
        put(path, content)
    report = run_once(scan, state)
    for path, content in contents.items():
        assert path.read_bytes() == content
        assert shared_range(a / "a1", CHUNK_SIZE, path, CHUNK_SIZE, CHUNK_SIZE), (path, report)
    assert shared_range(a / "a1", 0, b / "b2", 0, CHUNK_SIZE), report
    assert shared_range(a / "a2", 0, b / "b1", 0, CHUNK_SIZE), report
    print("Passed cross-file convergence when merged blocks have different first sources")


def test_completed_scan_pruning(root):
    scan, state, (a, b, c) = make_repositories(root, "scan-pruning", names=("a", "b", "c"))
    files = [put(repository / name, os.urandom(CHUNK_SIZE))
             for repository in (a, b, c) for name in ("one", "two")]
    before = {path: digest(path) for path in files}
    report = run_once(scan, state, 0)
    assert report["activity"]["hashedFiles"] == 6, report
    assert report["activity"]["repositories"] == 3, report
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT path FROM files")} == {os.fsencode(path) for path in files}

    removed = a / "one"
    removed.unlink()
    hardlink = root / "scan-pruning-hardlink"
    os.link(a / "two", hardlink)
    shutil.rmtree(c / ".git")
    retained = {b / "one", b / "two"}
    report = run_once(scan, state, 0)
    activity = report["activity"]
    assert activity["repositories"] == 2, activity
    assert activity["scannedFiles"] == 2, activity
    assert activity["hashedFiles"] == activity["processedFiles"] == 0, activity
    assert {project["path"] for project in report["projects"]} == {str(a), str(b)}, report
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT path FROM files")} == {os.fsencode(path) for path in retained}
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == {os.fsencode(a), os.fsencode(b)}
    assert {path: digest(path) for path in files if path != removed} == {
        path: checksum for path, checksum in before.items() if path != removed
    }
    assert digest(hardlink) == before[a / "two"]

    added = put(b / "new", os.urandom(CHUNK_SIZE))
    added_digest = digest(added)
    report = run_once(scan, state, 0)
    activity = report["activity"]
    assert activity["repositories"] == 2, activity
    assert activity["scannedFiles"] == 3, activity
    assert activity["hashedFiles"] == activity["processedFiles"] == 1, activity
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT path FROM files")} == {
            os.fsencode(path) for path in retained | {added}
        }
    assert all(digest(path) == before[path] for path in files if path != removed)
    assert digest(added) == added_digest
    assert digest(hardlink) == before[a / "two"]
    print("Passed completed-scan pruning, unchanged-file retention, and subsequent incremental indexing")


def test_artifact_discovery(root):
    scan = root / "artifact-discovery"
    scan.mkdir()
    state = root / "artifact-discovery-state"
    target = scan / "standalone" / "project" / "target"
    modules = scan / "packages" / "node_modules"
    nested_modules = modules / "pkg" / "node_modules" / "nested"
    repository = scan / "git-repository"
    plain = scan / "plain"
    template = root / "empty-template"
    template.mkdir(exist_ok=True)
    git("init", "-q", f"--template={template}", repository)
    (target / "debug").mkdir(parents=True)
    nested_modules.mkdir(parents=True)
    (repository / "target" / "debug").mkdir(parents=True)
    plain.mkdir()
    (scan / "target").symlink_to(target, target_is_directory=True)

    content = os.urandom(CHUNK_SIZE)
    target_file = put(target / "debug" / "common", content)
    modules_file = put(nested_modules / "common", content)
    repository_file = put(repository / "target" / "debug" / "common", content)
    plain_file = put(plain / "common", content)
    files = (target_file, modules_file, repository_file, plain_file)
    before = {path: digest(path) for path in files}
    expected_roots = {target, modules, repository}
    discovered = subprocess.check_output(
        [BINARY, "--root", scan, "--state-dir", state, "--discover"], text=True,
    ).splitlines()
    assert len(discovered) == 3 and set(discovered) == {str(path) for path in expected_roots}, discovered

    report = run_once(scan, state, 2 * len(content))
    assert report["activity"]["repositories"] == 3, report
    assert report["activity"]["scannedFiles"] == report["activity"]["hashedFiles"] == 3, report
    assert {project["path"] for project in report["projects"]} == {str(path) for path in expected_roots}, report
    assert shared_range(target_file, 0, modules_file, 0, len(content)), report
    assert shared_range(target_file, 0, repository_file, 0, len(content)), report
    assert not shared_range(target_file, 0, plain_file, 0, len(content)), report
    assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(plain_file))
    assert {path: digest(path) for path in files} == before
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(target_file), os.fsencode(target)),
            (os.fsencode(modules_file), os.fsencode(modules)),
            (os.fsencode(repository_file), os.fsencode(repository)),
        }
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == {
            os.fsencode(path) for path in expected_roots
        }
    print("Passed standalone artifact discovery, nested scope ownership, and plain-directory exclusion")


def test_language_artifact_discovery(root):
    scan = root / "language-artifacts"
    scan.mkdir()
    state = root / "language-artifacts-state"
    content = os.urandom(CHUNK_SIZE)
    members = {}
    names = (
        "target", "node_modules", ".venv", "__pycache__", "site-packages", ".gradle",
        ".dart_tool", "zig-out", ".build", "vendor", "Pods", ".nuxt",
    )
    for index, name in enumerate(names):
        scope = scan / f"language-{index:02d}" / name
        member = scope / "common"
        if name == ".venv":
            member = scope / "lib" / "site-packages" / "package" / "common"
        member.parent.mkdir(parents=True)
        if name == "vendor":
            (scope.parent / "composer.json").write_text('{"name":"test/vendor-fixture"}\n')
        members[put(member, content)] = scope
    for name, marker, marker_content in (
        ("custom-python-env", "pyvenv.cfg", "include-system-site-packages = false\n"),
        ("custom-cmake-output", "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release\n"),
    ):
        scope = scan / name
        scope.mkdir()
        (scope / marker).write_text(marker_content)
        members[put(scope / "common", content)] = scope

    project = scan / "manifest-project"
    project.mkdir()
    (project / "package.json").write_text('{"name":"artifact-test","private":true}\n')
    generic_names = ("build", "dist", "bin", "obj", "out", "artifacts")
    for name in generic_names:
        scope = project / name
        member = scope / "common"
        if name == "build":
            member = scope / "nested" / "target" / "common"
        member.parent.mkdir(parents=True)
        members[put(member, content)] = scope
    excluded = []
    for name in (*generic_names, "vendor", "targets", "targers", "target-copy"):
        directory = scan / "unmarked" / name
        directory.mkdir(parents=True)
        excluded.append(put(directory / "common", content))
    (scan / "unmarked" / "target").symlink_to(next(iter(members.values())), target_is_directory=True)

    expected_roots = {str(path) for path in members.values()}
    discovered = discover(scan, state)
    assert len(discovered) == len(expected_roots) and set(discovered) == expected_roots, discovered
    report = run_once(scan, state, (len(members) - 1) * len(content))
    assert report["activity"]["hashedFiles"] == len(members), report
    assert {project["path"] for project in report["projects"]} == expected_roots, report
    reference = next(iter(members))
    for path in members:
        assert path.read_bytes() == content
        assert shared_range(reference, 0, path, 0, len(content)), (path, report)
    for path in excluded:
        assert path.read_bytes() == content
        assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(path)), path
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(path), os.fsencode(scope)) for path, scope in members.items()
        }
    print("Passed language artifact names, environment/build markers, and manifest-gated generic directories")


def test_xdg_storage_containers(root):
    scan = root / "xdg-containers"
    scan.mkdir()
    env = os.environ.copy()
    containers = []
    for key, name in (
        ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
        ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache"),
    ):
        directory = scan / name
        directory.mkdir()
        containers.append(directory)
        env[key] = str(directory)
    state = scan / "state" / "project-bees"
    state.mkdir()
    content = os.urandom(CHUNK_SIZE)
    ignored = put(state / "do-not-index", content)
    plain = scan / "ordinary"
    plain.mkdir()
    excluded = put(plain / "common", content)
    members = {}
    for container in containers:
        members[put(container / "common", content)] = container
        for name in ("app-one", "app-two"):
            scope = container / name
            member = scope / "nested" / "files" / "common"
            member.parent.mkdir(parents=True)
            members[put(member, content)] = scope
        artifact_name = "node_modules" if container.name in {"config", "state"} else "target"
        artifact = container / "app-one" / "projects" / artifact_name
        member = artifact / "package" / "common"
        member.parent.mkdir(parents=True)
        members[put(member, content)] = artifact
    expected_roots = {str(path) for path in members.values()}
    discovered = discover(scan, state, env=env)
    assert len(discovered) == len(expected_roots) and set(discovered) == expected_roots, discovered
    report = run_once(scan, state, (len(members) - 1) * len(content), env=env)
    assert report["activity"]["hashedFiles"] == len(members), report
    assert {project["path"] for project in report["projects"]} == expected_roots, report
    reference = next(iter(members))
    for path in members:
        assert path.read_bytes() == content
        assert shared_range(reference, 0, path, 0, len(content)), (path, report)
    for path in (ignored, excluded):
        assert path.read_bytes() == content
        assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(path)), path
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(path), os.fsencode(scope)) for path, scope in members.items()
        }
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == {
            os.fsencode(path) for path in members.values()
        }
    print("Passed XDG root/direct-child sharing, nested ownership, and index-state exclusion")


def test_priority_pass_preserves_index(root):
    scan, state, (a, b) = make_repositories(root, "priority-pass", names=("aaa-old-a", "aaa-old-b"))
    x, y, z = (os.urandom(CHUNK_SIZE) for _ in range(3))
    old_x = put(a / "x", x)
    backlog_a = put(a / "backlog", y)
    backlog_b = put(b / "backlog", y)
    deleted = put(a / "deleted", z)
    run_once(scan, state, CHUNK_SIZE)
    with sqlite3.connect(state / "index.sqlite3") as db:
        old_ids = dict(db.execute("SELECT path,id FROM files"))
        assert db.execute(
            "UPDATE files SET pending=1 WHERE path IN (?,?)",
            (os.fsencode(backlog_a), os.fsencode(backlog_b)),
        ).rowcount == 2
    deleted.unlink()
    priority = scan / "zzz-priority"
    fresh = []
    for name in ("app-one", "app-two", "app-three"):
        directory = priority / name
        directory.mkdir(parents=True)
        fresh.append(put(directory / "x", x))
    lines = []
    report = run_once(scan, state, 4 * CHUNK_SIZE, priority_roots=(priority,), output=lines)
    priority_at = lines.index(f"priority scan root={priority}")
    full_at = lines.index("full scan")
    assert priority_at < full_at, lines
    early = [line for line in lines[priority_at:full_at] if line.startswith("dedup ranges=")]
    assert len(early) == 1, lines
    assert "dedup ranges=3 " in early[0], lines
    assert f"newly_shared_bytes={3 * CHUNK_SIZE} " in early[0], lines
    activity = report["activity"]
    assert activity["hashedFiles"] == activity["indexedChunks"] == 0, activity
    assert activity["scanKind"] == "full" and activity["scanRoot"] == "", activity
    events = report["recentEvents"]
    assert len(events) == len(fresh), events
    assert {event["source"] for event in events} == {str(old_x)}, events
    assert {event["destination"] for event in events} == {str(path) for path in fresh}, events
    assert sum(event["bytes"] for event in events) == 3 * CHUNK_SIZE, events
    # The full pass must drain all three old destinations deferred by priority work.
    assert activity["processedFiles"] >= 3 and activity["matchedRanges"] >= 3, activity
    retained = (old_x, backlog_a, backlog_b)
    with sqlite3.connect(state / "index.sqlite3") as db:
        final_ids = dict(db.execute("SELECT path,id FROM files"))
        assert set(final_ids) == {os.fsencode(path) for path in (*retained, *fresh)}
        assert all(final_ids[os.fsencode(path)] == old_ids[os.fsencode(path)] for path in retained)
        assert db.execute("SELECT count(*) FROM files WHERE pending<>0").fetchone()[0] == 0
    assert old_x.read_bytes() == x
    assert backlog_a.read_bytes() == backlog_b.read_bytes() == y
    assert shared_range(backlog_a, 0, backlog_b, 0, CHUNK_SIZE)
    for path in fresh:
        assert path.read_bytes() == x
        assert shared_range(old_x, 0, path, 0, CHUNK_SIZE), (path, report)
    print("Passed priority-before-full execution, early sharing, index retention, and deferred old pending work")


def test_nested_repository_priority(root):
    scan, state, (a, b) = make_repositories(root, "nested-priority")
    priority = a / "target"
    (priority / "debug").mkdir(parents=True)
    content = os.urandom(CHUNK_SIZE)
    existing = put(priority / "debug" / "old", content)
    reference = put(b / "common", content)
    run_once(scan, state, CHUNK_SIZE)
    with sqlite3.connect(state / "index.sqlite3") as db:
        old_ids = dict(db.execute("SELECT path,id FROM files"))
    fresh = put(priority / "debug" / "new", content)
    discovered = discover(scan, state, priority_roots=(priority,))
    assert len(discovered) == 2 and set(discovered) == {str(a), str(b)}, discovered
    lines = []
    report = run_once(scan, state, 2 * CHUNK_SIZE, priority_roots=(priority,), output=lines)
    assert lines.index(f"priority scan root={priority}") < lines.index("full scan"), lines
    assert report["activity"]["hashedFiles"] == 0, report
    assert {project["path"] for project in report["projects"]} == {str(a), str(b)}, report
    with sqlite3.connect(state / "index.sqlite3") as db:
        final_ids = dict(db.execute("SELECT path,id FROM files"))
        assert all(final_ids[path] == identity for path, identity in old_ids.items())
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(existing), os.fsencode(a)),
            (os.fsencode(fresh), os.fsencode(a)),
            (os.fsencode(reference), os.fsencode(b)),
        }
        assert {row[0] for row in db.execute("SELECT path FROM repositories")} == {os.fsencode(a), os.fsencode(b)}
    for path in (existing, reference, fresh):
        assert path.read_bytes() == content
        assert shared_range(reference, 0, path, 0, CHUNK_SIZE), (path, report)
    print("Passed nested Git artifact priority with stable repository roots and file IDs")


def test_outside_priority_is_ignored(root):
    scan, state, (repository,) = make_repositories(root, "outside-priority", names=("inside",))
    content = os.urandom(CHUNK_SIZE)
    inside = put(repository / "common", content)
    priority = root / "outside-priority-extra"
    priority.mkdir()
    outside = put(priority / "common", content)
    discovered = discover(scan, state, priority_roots=(priority,))
    assert discovered == [str(repository)], discovered
    lines = []
    report = run_once(scan, state, 0, priority_roots=(priority,), output=lines)
    assert not any(line.startswith("priority scan root=") for line in lines), lines
    assert report["activity"]["hashedFiles"] == 1, report
    assert {project["path"] for project in report["projects"]} == {str(repository)}, report
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {(os.fsencode(inside), os.fsencode(repository))}
    assert inside.read_bytes() == outside.read_bytes() == content
    assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(outside))
    print("Passed explicit scan boundaries without widening them for an outside priority directory")


def test_backup_files(root):
    scan = root / "named-file-fixtures"
    scan.mkdir()
    state = scan / "self.bak"
    state.mkdir()
    pairs = []
    members = set()
    variants = (
        lambda name: name + ".bak", lambda name: name + ".BACKUP",
        lambda name: name + ".old", lambda name: name + ".orig",
        lambda name: name + ".save", lambda name: name + "~",
        lambda name: "Backup-" + name, lambda name: name + "_BACKUPS",
        lambda name: name + ".bak.2026-11-21_123045",
        lambda name: name + ".20261121.BAK",
    )
    for index, variant in enumerate(variants):
        content = os.urandom(CHUNK_SIZE)
        original = put(scan / f"document-{index}.bin", content)
        backup = put(scan / variant(original.name), content)
        pairs.append((original, backup, content))
        members.update((original, backup))

    dated_content = os.urandom(CHUNK_SIZE)
    dated_original = put(scan / "report.20261121", dated_content)
    dated_backup = put(scan / "report.20261121.bak", dated_content)
    pairs.append((dated_original, dated_backup, dated_content))
    members.update((dated_original, dated_backup))

    unrelated = pairs[0][2]
    excluded = [put(scan / name, unrelated) for name in (
        "ordinary", "old", "copy", "bakery", "old-release", "release-copy", "release.20261121",
    )]
    excluded.append(put(scan / "report", dated_content))
    (scan / "self").mkdir()
    excluded.extend((put(state / "ignored.bak", unrelated), put(scan / "self" / "ignored", unrelated)))
    linked = put(scan / "hardlinked", unrelated)
    os.link(linked, scan / "hardlinked.bak")
    outside = put(root / "unscanned-file", os.urandom(CHUNK_SIZE))
    (scan / "alias").symlink_to(outside)
    members.add(put(scan / "alias.bak", outside.read_bytes()))
    (scan / "symlinked.bak").symlink_to(outside)
    excluded.append(put(scan / "symlinked", outside.read_bytes()))
    members.add(put(scan / "orphan.BAK", os.urandom(CHUNK_SIZE)))
    before = {path: digest(path) for path in members | set(excluded) | {linked, outside}}

    discovered = discover(scan, state)
    assert len(discovered) == len(members) and set(discovered) == {str(path) for path in members}, discovered
    report = run_once(scan, state, len(pairs) * CHUNK_SIZE)
    for original, backup, content in pairs:
        assert original.read_bytes() == backup.read_bytes() == content
        assert shared_range(original, 0, backup, 0, len(content)), (original, report)
    for path in (*excluded, outside):
        assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(path)), path
    assert before == {path: digest(path) for path in before}
    assert linked.stat().st_nlink == 2
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(path), os.fsencode(path)) for path in members
        }
        previous = dict(db.execute("SELECT path,id FROM files"))
    repeated = run_once(scan, state, len(pairs) * CHUNK_SIZE)
    assert repeated["activity"]["hashedFiles"] == 0, repeated
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert dict(db.execute("SELECT path,id FROM files")) == previous

    original, backup, content = pairs[0]
    backup.unlink()
    run_once(scan, state, len(pairs) * CHUNK_SIZE)
    with sqlite3.connect(state / "index.sqlite3") as db:
        indexed = {row[0] for row in db.execute("SELECT path FROM files")}
        assert os.fsencode(original) not in indexed and os.fsencode(backup) not in indexed
    assert original.read_bytes() == content

    # A retained original must lose eligibility immediately when its sole
    # witness gains a hardlink, before a partial pass can use the old index.
    original, backup, content = pairs[1]
    os.link(backup, root / "witness-hardlink")
    priority = scan / "priority"
    priority.mkdir()
    fresh = put(priority / "common", content)
    lines = []
    report = run_once(scan, state, len(pairs) * CHUNK_SIZE, priority_roots=(priority,), output=lines)
    assert report["recentEvents"] == [], report
    assert not shared_range(original, 0, fresh, 0, CHUNK_SIZE)
    assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(fresh))
    assert original.read_bytes() == backup.read_bytes() == fresh.read_bytes() == content
    early = [line for line in lines[:lines.index("full scan")] if line.startswith("dedup ranges=")]
    assert len(early) == 1 and "dedup ranges=0 " in early[0], lines
    with sqlite3.connect(state / "index.sqlite3") as db:
        indexed = {row[0] for row in db.execute("SELECT path FROM files")}
        assert os.fsencode(original) not in indexed and os.fsencode(backup) not in indexed
        assert os.fsencode(fresh) in indexed
    print("Passed named backup files, exact siblings, hardlinked witness invalidation, and bounded discovery")


def test_backup_scopes(root):
    scan, state, (repository,) = make_repositories(root, "named-scope-fixtures", names=("repository",))
    target = scan / "standalone" / "target"
    target.mkdir(parents=True)
    members = {}
    pairs = []

    def pair(original, backup, original_scope, backup_scope):
        content = os.urandom(CHUNK_SIZE)
        for path, scope in ((original, original_scope), (backup, backup_scope)):
            path.parent.mkdir(parents=True, exist_ok=True)
            members[put(path, content)] = scope
        pairs.append((original, backup, content))

    project = scan / "project"
    snapshot = scan / "project.BAK-20261121"
    pair(project / "nested" / "common", snapshot / "nested" / "common", project, snapshot)
    dated_project = scan / "project-2026-11-21"
    dated_snapshot = scan / "project-2026-11-21.bak"
    pair(dated_project / "common", dated_snapshot / "common", dated_project, dated_snapshot)
    pair(repository / "config", repository / "config.bak", repository, repository / "config.bak")
    pair(repository / "output" / "common", repository / "output.old" / "common",
         repository, repository / "output.old")
    pair(target / "module", target / "module~", target, target / "module~")
    pair(target / "result" / "common", target / "result.backup" / "common", target, target / "result.backup")
    containers = []
    for name in ("Backups", ".backups", "备份"):
        container = scan / name
        containers.append(container)
        pair(container / "first" / "common", container / "second" / "common",
             container / "first", container / "second")
    excluded = []
    for name in ("unrelated", "old", "copy", "bakery"):
        directory = scan / name
        directory.mkdir()
        excluded.append(put(directory / "common", pairs[0][2]))
    (scan / "alias.bak").symlink_to(project, target_is_directory=True)
    expected_scopes = set(members.values()) | set(containers)
    discovered = discover(scan, state)
    assert len(discovered) == len(expected_scopes) and set(discovered) == {
        str(path) for path in expected_scopes
    }, discovered
    report = run_once(scan, state, len(pairs) * CHUNK_SIZE)
    for original, backup, content in pairs:
        assert original.read_bytes() == backup.read_bytes() == content
        assert shared_range(original, 0, backup, 0, len(content)), (original, report)
    for path in excluded:
        assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(path)), path
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(path), os.fsencode(scope)) for path, scope in members.items()
        }
        previous = dict(db.execute("SELECT path,id FROM files"))
    # Entering a paired original below its root must retain the full walk's owner.
    repeated = run_once(scan, state, len(pairs) * CHUNK_SIZE, priority_roots=(project / "nested",))
    assert repeated["activity"]["hashedFiles"] == 0, repeated
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert dict(db.execute("SELECT path,id FROM files")) == previous

    boundary = root / "named-boundary-fixtures"
    original = boundary / "project"
    backup = boundary / "project.bak"
    original.mkdir(parents=True)
    backup.mkdir()
    content = os.urandom(CHUNK_SIZE)
    source = put(original / "common", content)
    destination = put(backup / "common", content)
    limited_state = root / "named-boundary-state"
    run_once(backup, limited_state, 0)
    assert not shared_range(source, 0, destination, 0, CHUNK_SIZE)
    with sqlite3.connect(limited_state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path FROM files")) == {(os.fsencode(destination),)}
    run_once(boundary, limited_state, CHUNK_SIZE)
    assert shared_range(source, 0, destination, 0, CHUNK_SIZE)
    assert source.read_bytes() == destination.read_bytes() == content

    rotation = root / "witness-rotation-fixtures"
    project = rotation / "project"
    nested = project / "sub"
    nested.mkdir(parents=True)
    (rotation / "project.old").mkdir()
    content = os.urandom(CHUNK_SIZE)
    first = put(project / "first", content)
    second = put(nested / "second", content)
    rotation_state = root / "witness-rotation-state"
    run_once(rotation, rotation_state, 0)
    assert not shared_range(first, 0, second, 0, CHUNK_SIZE)
    # A smaller witness changes only the key of the priority subtree first.
    # Old and new evidence for one root must not permit intra-scope dedup.
    (rotation / "project.bak").mkdir()
    report = run_once(rotation, rotation_state, 0, priority_roots=(nested,))
    assert report["recentEvents"] == [] and report["pairs"] == [], report
    assert not shared_range(first, 0, second, 0, CHUNK_SIZE)
    assert first.read_bytes() == second.read_bytes() == content
    with sqlite3.connect(rotation_state / "index.sqlite3") as db:
        assert {row[0] for row in db.execute("SELECT DISTINCT root FROM files")} == {os.fsencode(project)}
    print("Passed backup directories, nested copies, witness rotation, priority ownership, and root boundaries")


def test_readme_discovery(root):
    scan, state, (repository,) = make_repositories(root, "readme-project-fixtures", names=("repository",))
    standalone = scan / "standalone"
    nested = repository / "docs"
    standalone.mkdir()
    nested.mkdir()
    for directory, name in ((repository, "README.md"), (standalone, "rEaDmE.mD"), (nested, "readme.md")):
        (directory / name).write_text("Project description.\n")
    content, later = os.urandom(CHUNK_SIZE), os.urandom(CHUNK_SIZE)
    source = put(repository / "common", content)
    destination = put(standalone / "common", content)
    nested_file = put(nested / "common", later)
    same_scope = put(repository / "same-scope", later)
    ignored = scan / "plain"
    ignored.mkdir()
    (ignored / "README.md").symlink_to(repository / "README.md")
    excluded = put(ignored / "common", content)
    expected = {str(repository), str(standalone)}
    assert set(discover(scan, state)) == expected
    report = run_once(scan, state, CHUNK_SIZE)
    assert shared_range(source, 0, destination, 0, CHUNK_SIZE), report
    assert not shared_range(nested_file, 0, same_scope, 0, CHUNK_SIZE)
    with sqlite3.connect(state / "index.sqlite3") as db:
        previous = list(db.execute("SELECT path,id,root,repo FROM files ORDER BY path"))
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(source), os.fsencode(repository)),
            (os.fsencode(destination), os.fsencode(standalone)),
            (os.fsencode(nested_file), os.fsencode(repository)),
            (os.fsencode(same_scope), os.fsencode(repository)),
        }
    shutil.rmtree(repository / ".git")
    assert set(discover(scan, state)) == expected
    repeated = run_once(scan, state, CHUNK_SIZE)
    assert repeated["activity"]["hashedFiles"] == 0, repeated
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert list(db.execute("SELECT path,id,root,repo FROM files ORDER BY path")) == previous
    fresh = put(standalone / "after-removal", later)
    report = run_once(scan, state, 3 * CHUNK_SIZE)
    for path in (nested_file, same_scope, fresh):
        assert path.read_bytes() == later
        assert shared_range(fresh, 0, path, 0, CHUNK_SIZE), (path, report)
    assert source.read_bytes() == destination.read_bytes() == excluded.read_bytes() == content
    assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(excluded))
    print("Passed case-insensitive regular README markers and stable ownership after Git metadata removal")


def test_opt_storage(root):
    scan = root / "application-fixtures"
    home = scan / "home"
    home.mkdir(parents=True)
    state = root / "application-fixtures-state"
    env = os.environ.copy()
    env["HOME"] = str(home)
    content = os.urandom(CHUNK_SIZE)
    members = {}
    for container in (home / "opt", home / ".local" / "opt"):
        container.mkdir(parents=True)
        (container / "README.md").write_text("Application container.\n")
        members[put(container / "common", content)] = container
        for name in ("app-one", "app-two"):
            scope = container / name
            member = scope / "nested" / "common"
            member.parent.mkdir(parents=True)
            if name == "app-one":
                (scope / "README.md").write_text("Application project.\n")
                (member.parent / "README.md").write_text("Application documentation.\n")
            members[put(member, content)] = scope
    excluded = put(home / "unrelated", content)
    expected_scopes = {str(path) for path in members.values()}
    assert set(discover(scan, state, env=env)) == expected_scopes
    report = run_once(scan, state, (len(members) - 1) * CHUNK_SIZE, env=env)
    reference = next(iter(members))
    for path in members:
        assert path.read_bytes() == content
        assert shared_range(reference, 0, path, 0, CHUNK_SIZE), (path, report)
    with sqlite3.connect(state / "index.sqlite3") as db:
        assert set(db.execute("SELECT path,root FROM files")) == {
            (os.fsencode(path), os.fsencode(scope)) for path, scope in members.items()
        }
    assert excluded.read_bytes() == content
    assert all((extent[5] & FIEMAP_EXTENT_SHARED) == 0 for extent in extents(excluded))
    print("Passed opt application roots and per-application scopes without widening the home directory")


def test_recent_events_limit(root):
    scan, state, (a, b) = make_repositories(root, "recent-events-limit")
    content = os.urandom(CHUNK_SIZE)
    files = [put(repository / f"copy-{index:02d}", content)
             for repository in (a, b) for index in range(18)]
    report = run_once(scan, state, 35 * CHUNK_SIZE)
    events = report["recentEvents"]
    assert len(events) == 32, events
    assert len({event["destination"] for event in events}) == 32, events
    expected_files = {str(path) for path in files}
    assert all(event["source"] in expected_files and event["destination"] in expected_files for event in events), events
    assert all(event["bytes"] == CHUNK_SIZE for event in events), events
    assert sum(event["bytes"] for event in events) == 32 * CHUNK_SIZE, events
    for path in files:
        assert path.read_bytes() == content
        assert shared_range(files[0], 0, path, 0, CHUNK_SIZE), (path, report)
    print("Passed bounded recent events while preserving cumulative sharing and per-file identity")


if __name__ == "__main__":
    test_parent = Path(os.environ.get("PROJECT_BEES_TEST_DIR", BINARY.parents[1])).resolve()
    fstype = subprocess.check_output(["stat", "-f", "-c", "%T", test_parent], text=True).strip()
    if fstype != "btrfs":
        raise SystemExit("Set PROJECT_BEES_TEST_DIR to a writable directory on Btrfs")
    with tempfile.TemporaryDirectory(prefix="project-bees-test-", dir=test_parent) as directory:
        test(Path(directory))
        test_stats_encoding(Path(directory))
        test_coalesced_ranges(Path(directory))
        test_shifted_and_sparse_ranges(Path(directory))
        test_coalesced_hash_collision(Path(directory))
        test_nontransitive_pending(Path(directory))
        test_repeated_block_convergence(Path(directory))
        test_cross_file_source_convergence(Path(directory))
        test_completed_scan_pruning(Path(directory))
        test_artifact_discovery(Path(directory))
        test_language_artifact_discovery(Path(directory))
        test_xdg_storage_containers(Path(directory))
        test_priority_pass_preserves_index(Path(directory))
        test_nested_repository_priority(Path(directory))
        test_outside_priority_is_ignored(Path(directory))
        test_backup_files(Path(directory))
        test_backup_scopes(Path(directory))
        test_readme_discovery(Path(directory))
        test_opt_storage(Path(directory))
        test_recent_events_limit(Path(directory))
