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


def make_repositories(root, name, names=("a", "b")):
    scan = root / name
    scan.mkdir()
    template = root / "empty-template"
    template.mkdir(exist_ok=True)
    repositories = [scan / name for name in names]
    for repository in repositories:
        git("init", "-q", f"--template={template}", repository)
    return scan, root / (name + "-state"), repositories


def run_once(scan, state, expected=None):
    process = subprocess.run(
        [BINARY, "--root", scan, "--state-dir", state, "--once"],
        text=True, capture_output=True,
    )
    assert process.returncode == 0, process.stdout + process.stderr
    report = json.loads(subprocess.check_output([BINARY, "--state-dir", state, "--stats"]))
    check_activity(report, "stopped")
    total = report["totalBytes"]
    if expected is not None:
        assert total == expected, (report, process.stdout)
    assert sum(project["bytes"] for project in report["projects"]) == total
    assert sum(project["sourceBytes"] for project in report["projects"]) == total
    assert sum(pair["bytes"] for pair in report["pairs"]) == total
    return report


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
    os.utime(source, None)
    repeated = run_once(scan, state, len(content))
    assert repeated["rangeCount"] == report["rangeCount"], repeated
    assert repeated["startedAt"] == report["startedAt"], repeated
    assert repeated["activity"]["hashedFiles"] == 1, repeated
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
