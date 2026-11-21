"""Compare two project-bees binaries on independent, reproducible Btrfs datasets.

Example:
    python3 test/project-bees-benchmark.py --before /tmp/project-bees-before \
        --after bin/project-bees --directory /path/on/btrfs --repeats 3

Each invocation receives freshly written files and its own index. Each binary
performs its own incremental setup; cumulative statistics must agree across
versions, including that setup. Files are never reflinked.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import selectors
import signal
import statistics
import subprocess
import sys
import tempfile
import time


CHUNK_SIZE = 128 * 1024
SEED = 20261121
TIMEOUT_SECONDS = 180
METRICS = (
    "wallSeconds", "dedupSeconds", "matchedRanges", "rejectedRanges",
    "totalBytes", "newlySharedBytes",
)


def positive_int(value):
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("repeats must be at least 1")
    return result


def run_daemon(binary, scan, state):
    command = [str(binary), "--root", str(scan), "--state-dir", str(state), "--once"]
    output = {"stdout": bytearray(), "stderr": bytearray()}
    pending_line = bytearray()
    scan_finished = None
    dedup_finished = None
    started = time.perf_counter()
    deadline = started + TIMEOUT_SECONDS

    with subprocess.Popen(
        command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=True,
    ) as process, selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        selector.register(process.stderr, selectors.EVENT_READ, "stderr")
        try:
            while selector.get_map():
                remaining = deadline - time.perf_counter()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, TIMEOUT_SECONDS)
                for key, _ in selector.select(min(remaining, 1)):
                    data = os.read(key.fileobj.fileno(), 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    output[key.data].extend(data)
                    if key.data == "stdout":
                        pending_line.extend(data)
                        while b"\n" in pending_line:
                            line, _, rest = pending_line.partition(b"\n")
                            pending_line = bytearray(rest)
                            observed = time.perf_counter()
                            if line.startswith(b"scan repositories="):
                                scan_finished = observed
                            elif line.startswith(b"dedup ranges="):
                                dedup_finished = observed
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(command, TIMEOUT_SECONDS)
            process.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(
                f"{binary}: exceeded {TIMEOUT_SECONDS}s\n"
                f"stdout:\n{output['stdout'].decode('utf-8', errors='replace')}\n"
                f"stderr:\n{output['stderr'].decode('utf-8', errors='replace')}"
            ) from error
        elapsed = time.perf_counter() - started

    stdout = output["stdout"].decode("utf-8", errors="replace")
    stderr = output["stderr"].decode("utf-8", errors="replace")
    if process.returncode != 0:
        raise RuntimeError(f"{binary}: exit {process.returncode}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    if scan_finished is None or dedup_finished is None or dedup_finished < scan_finished:
        raise RuntimeError(f"{binary}: missing scan/dedup completion lines\nstdout:\n{stdout}\nstderr:\n{stderr}")
    try:
        report = json.loads((state / "stats.json").read_text(encoding="utf-8"))
        activity = report["activity"]
        if activity["phase"] != "stopped":
            raise ValueError("the one-shot run did not publish a stopped snapshot")
        for value in (report["totalBytes"], activity["matchedRanges"], activity["rejectedRanges"]):
            if type(value) is not int or value < 0:
                raise ValueError("invalid statistics counter")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError(f"{binary}: invalid statistics: {error}\nstdout:\n{stdout}\nstderr:\n{stderr}") from error
    return {
        "wallSeconds": elapsed,
        "dedupSeconds": dedup_finished - scan_finished,
        "matchedRanges": activity["matchedRanges"],
        "rejectedRanges": activity["rejectedRanges"],
        "totalBytes": report["totalBytes"],
    }


def repositories(root):
    scan = root / "scan"
    scan.mkdir()
    template = root / "empty-template"
    template.mkdir()
    result = []
    for index in range(8):
        repository = scan / f"repo-{index:02d}"
        command = ["git", "init", "-q", f"--template={template}", str(repository)]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=TIMEOUT_SECONDS)
        if completed.returncode != 0:
            raise RuntimeError(f"git init failed for {repository}: {completed.stderr}")
        result.append(repository)
    return scan, result


def write_files(repos, prefix, count, content, expected):
    checksum = hashlib.sha256(content).digest()
    for repository in repos:
        for index in range(count):
            path = repository / f"{prefix}-{index:03d}.bin"
            with path.open("wb") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
            expected[path] = checksum


def check_contents(expected):
    for path, checksum in expected.items():
        actual = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                actual.update(block)
        if actual.digest() != checksum:
            raise AssertionError(f"file content changed: {path}")


def measure(binary, directory, scenario, content):
    with tempfile.TemporaryDirectory(prefix=f"project-bees-benchmark-{scenario}-", dir=directory) as temp:
        root = Path(temp)
        scan, repos = repositories(root)
        state = root / "state"
        expected = {}
        previous_bytes = 0
        if scenario == "contiguous":
            write_files(repos, "common", 1, content, expected)
            expected_added = (len(repos) - 1) * len(content)
        else:
            write_files(repos, "old", 16, content, expected)
            previous_bytes = run_daemon(binary, scan, state)["totalBytes"]
            write_files(repos, "new", 8, content, expected)
            expected_added = len(repos) * 8 * len(content)
        result = run_daemon(binary, scan, state)
        result["newlySharedBytes"] = result["totalBytes"] - previous_bytes
        if result["newlySharedBytes"] != expected_added:
            raise AssertionError(
                f"{binary}: {scenario}: expected {expected_added} newly shared bytes: {result}"
            )
        check_contents(expected)
        return result


def benchmark(before, after, directory, repeats):
    random_data = random.Random(SEED)
    datasets = [
        ("contiguous", random_data.randbytes(4 * 1024 * 1024 + 22133)),
        ("incremental-repeated-chunks", random_data.randbytes(CHUNK_SIZE) * 16),
    ]
    binaries = {"before": before, "after": after}
    result = {
        "seed": SEED,
        "repeats": repeats,
        "timeoutSeconds": TIMEOUT_SECONDS,
        "binaries": {name: str(path) for name, path in binaries.items()},
        "incrementalSetup": "each binary performs its own setup",
        "timing": "dedupSeconds measures receipt of the final scan line to receipt of the dedup line",
        "scenarios": {},
    }
    for scenario_index, (scenario, content) in enumerate(datasets):
        runs = []
        for repeat in range(repeats):
            order = ["before", "after"] if (repeat + scenario_index) % 2 == 0 else ["after", "before"]
            sample = {"repeat": repeat + 1, "order": order}
            for name in order:
                sample[name] = measure(binaries[name], directory, scenario, content)
            for metric in ("totalBytes", "newlySharedBytes"):
                if sample["before"][metric] != sample["after"][metric]:
                    raise AssertionError(f"{scenario} repeat {repeat + 1}: unequal {metric}: {sample}")
            runs.append(sample)
        result["scenarios"][scenario] = {
            "dataset": {
                "repositories": 8,
                "oldFiles": 0 if scenario == "contiguous" else 128,
                "newFiles": 8 if scenario == "contiguous" else 64,
                "fileBytes": len(content),
            },
            "runs": runs,
            "median": {
                name: {metric: statistics.median(run[name][metric] for run in runs) for metric in METRICS}
                for name in binaries
            },
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--repeats", type=positive_int, default=3)
    args = parser.parse_args()
    before, after, directory = args.before.resolve(), args.after.resolve(), args.directory.resolve()
    for binary in (before, after):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"binary must be an executable file: {binary}")
    if not directory.is_dir():
        parser.error(f"directory does not exist: {directory}")
    fstype = subprocess.check_output(["stat", "-f", "-c", "%T", str(directory)], text=True).strip()
    if fstype != "btrfs":
        parser.error("--directory must be a writable directory on Btrfs")
    print(json.dumps(benchmark(before, after, directory, args.repeats), indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"project-bees benchmark: {error}", file=sys.stderr)
        raise SystemExit(1)
