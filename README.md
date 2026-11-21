# project-bees

`project-bees` is a fork of [Zygo/bees](https://github.com/Zygo/bees) that
shares identical Btrfs file ranges across local Git repositories, common language
build and dependency directories, package caches, and user application data.
It runs as a regular user and uses the
upstream CityHash and Btrfs extent-same code. Btrfs
compares the data in the kernel before sharing storage. Subsequent writes use
copy-on-write.

## Install on Arch Linux

Install the [AUR package](https://aur.archlinux.org/packages/project-bees) and
start the user service:

```sh
yay -S project-bees
systemctl --user enable --now project-bees.service
project-bees --stats
```

The package installs `/usr/bin/project-bees` and
`/usr/lib/systemd/user/project-bees.service`. When upgrading from
`make install-user`, stop the service and remove its old
`~/.config/systemd/user/project-bees.service` override, then run
`systemctl --user daemon-reload` before starting the packaged service. Keep the
state directory to retain the index and accumulated statistics.

### Omarchy dashboard

The [Project Bees plugin](https://github.com/LIghtJUNction/omarchy-project-bees)
adds a native bar widget and a searchable dashboard for total newly shared
data, per-repository totals, and repository pairs:

```sh
omarchy plugin add https://github.com/LIghtJUNction/omarchy-project-bees --enable
```

Click its bar widget or run
`omarchy-shell shell summon io.github.lightjunction.project-bees` to open it.
The dashboard also shows whether the latest snapshot reports indexing,
deduplication, a wait before the next scan, or a stopped daemon. With plugin
0.1.3, its progress area shows the priority/full pass, current priority directory,
file and chunk counts, and phase age. Its live log shows the latest actual
file-to-file sharing records with their newly shared bytes.
The plugin is independently maintained under the MIT license and pinned in
this repository at `plugins/project-bees` as a Git submodule. It reads the
daemon's local statistics snapshot.

## Build and run

Build dependencies are GNU make, a C++17 compiler, and SQLite 3.38 or later development
headers. Runtime requires Linux 5.6 or later and writable Btrfs. Python 3 and
Git are used by the integration test.

```sh
make
bin/project-bees --discover
bin/project-bees --once
```

`--discover` lists the discovered scope roots without creating an index.
`--once` performs one scan cycle, including its priority passes. With neither flag the daemon
waits five minutes after each pass, then picks up new repositories, changed
files, and deletions. Use `--interval SECONDS` to change that delay, or repeat
`--root DIR` to limit where it looks for scopes. A limited scan uses one full
pass by default. Add `--priority-root DIR` to process a directory inside that
limit earlier; the option never widens an explicit `--root` limit.

### Scan order and language directories

Default system scans process `/var/tmp` and common language caches first, then
the XDG cache, data, configuration and state directories, followed by a complete
discovery pass. Each priority directory is scanned and deduplicated before the
next one. This order is a heuristic based on where repeated builds and installed
dependencies accumulate; contents still determine whether a match is valid.

| Area | Discovery examples |
| --- | --- |
| JavaScript / TypeScript | `node_modules`, `.yarn`, `.next`, `.nuxt`, `.svelte-kit`, `.parcel-cache`, `.turbo`; npm, Yarn, pnpm and Bun caches |
| Rust / JVM | `target`, `.gradle`; Cargo registry/Git, Rustup toolchains, Maven, Gradle, Ivy and Coursier caches |
| Python | `.venv`, `venv`, `site-packages`, `__pycache__`, `.tox`, `.nox`; pip, uv and Poetry caches |
| C/C++ / .NET | CMake build trees; manifest-qualified `build`, `bin`, `obj`, `out`, `artifacts`; ccache, sccache and NuGet caches |
| Other language ecosystems | `.dart_tool`, `Pods`, `.build`, Zig caches/output, `_build`, `.stack-work`, `dist-newstyle`, `_opam`; Composer, Bundler, Go, Pub and other package caches |
| User application files | `${XDG_CACHE_HOME:-~/.cache}`, `${XDG_DATA_HOME:-~/.local/share}`, `${XDG_CONFIG_HOME:-~/.config}`, `${XDG_STATE_HOME:-~/.local/state}` |

An arbitrary directory containing `pyvenv.cfg` or `CMakeCache.txt` is also
recognized. Generic names such as `build`, `dist`, `bin`, `obj`, `vendor` and
`deps` require a supported project manifest in that directory or its parent.
This avoids treating every system `bin` or unrelated `build` directory as a
project output tree. Relevant absolute cache environment variables are honored;
relative XDG paths fall back to the default user directories.

The directory conventions follow the [XDG specification](https://specifications.freedesktop.org/basedir/latest/),
[Python virtual-environment documentation](https://docs.python.org/3/library/venv.html),
[CMake build-tree documentation](https://cmake.org/cmake/help/latest/manual/cmake.1.html),
and [Cargo home documentation](https://doc.rust-lang.org/cargo/guide/cargo-home.html).

To prioritize another directory while retaining a limited full pass:

```sh
project-bees --root "$HOME/work" --priority-root "$HOME/work/app/target" --once
```

A priority subtree inherits the same Git or artifact owner as the full pass.
Partial scans retain other indexed files and directories; only the completed
full scan prunes missing entries. Priority deduplication processes the current
batch, while the existing index remains available for finding source files.
Other pending work is left for the final full pass. An unchanged file may be
visited again in the full pass, but its cached content hash is retained.

### Run in the background

```sh
make install-user
systemctl --user daemon-reload
systemctl --user enable --now project-bees.service
journalctl --user -u project-bees.service -f
```

The service runs with low CPU and I/O priority and a soft memory limit of
256 MiB, including filesystem cache charged to the service. The default index
directory is `${XDG_STATE_HOME:-$HOME/.local/state}/project-bees`. Use
`--state-dir DIR` to keep separate indexes for different scans.

## Deduplication statistics

`project-bees --stats` prints the latest `stats.json` from the state directory.
The daemon stores cumulative counters in SQLite and atomically replaces the
JSON snapshot after completed scan and deduplication batches. Readers can
inspect it while the service is running. The dashboard refreshes on file
changes and checks again every 30 seconds.

The counters measure **newly shared logical bytes**. For each successful
deduplication range, the daemon compares FIEMAP extents before and after the
operation and counts allocated destination bytes that changed from exclusive
to shared. Already shared extents add zero. Each counted byte is credited to
the destination repository and to the pair of source and destination
repositories; a source subtotal records the other side of the operation.

| JSON field | Meaning |
| --- | --- |
| `version` | Snapshot format version, currently `1` |
| `startedAt`, `updatedAt` | Collection start and snapshot time, as Unix seconds |
| `totalBytes` | Cumulative newly shared logical bytes |
| `rangeCount` | Successful ranges with a positive newly shared byte count |
| `projects[].path` | Repository path |
| `projects[].bytes` | Bytes credited to this repository as destination |
| `projects[].sourceBytes` | Bytes credited to other repositories using this source |
| `pairs[].a`, `pairs[].b` | The two repository paths in byte order |
| `pairs[].bytes` | Pair total across both directions |
| `pairs[].toA` | Bytes credited to `a`; the remainder is credited to `b` |
| `recentEvents[]` | Up to 32 recent positive sharing records from the current daemon process |

Each recent event contains `at` (Unix seconds), `source` and `destination`
file paths, and `bytes`. Consecutive newly shared ranges for the same file pair
are combined into one record. Already shared matches add no event. Records
survive scan-phase changes but reset when the daemon restarts; they are a bounded
live view, while the cumulative totals remain persistent.

The project destination totals, project source totals, and pair totals each
sum to `totalBytes`. Repository and pair lists are sorted by credited bytes.
History survives restarts and repository removal. Tracking begins when this
version first opens the state directory; it does not reconstruct earlier
deduplication. New writes can make a previously shared range exclusive again,
so these cumulative counters do not measure current disk savings. Compression,
snapshots, allocation, and later writes also affect physical space use.

Paths remain byte-exact in the index. For display, each invalid UTF-8 path byte
is replaced by U+FFFD in JSON. Valid Unicode, quotes, backslashes, and control
characters remain valid JSON strings. The snapshot is private to the service's
user and contains local repository paths.

### Activity and an initial total of zero

Each pass first indexes its scope files, then deduplicates matching ranges.
Priority passes can produce shared data before the full discovery pass finishes.
The first index can take a long time on a large directory tree.
During that stage, the newly shared total can stay at zero while the file and
chunk counters grow. A zero total by itself does not show whether the daemon
is working. The default 300-second interval is the delay **after both stages
finish**, not a limit on indexing time.

Since 0.1.1, snapshots also include an optional `activity` object. Its counters
describe the current pass and reset when a new scan starts; the cumulative
statistics above keep their history.

| Activity field | Meaning |
| --- | --- |
| `phase` | `scanning`, `deduplicating`, `idle`, or `stopped` |
| `scanKind` | `priority` or `full` (added in 0.1.3) |
| `scanRoot` | Current priority directory, or an empty string during the full pass |
| `phaseStartedAt` | Time this phase began, as Unix seconds |
| `repositories` | Git or standalone build/dependency scope visits in the current scan |
| `scannedFiles` | Eligible file visits in the current scan, including cached files |
| `hashedFiles` | Files successfully read and indexed in this scan |
| `indexedChunks` | Nonzero chunks hashed in this scan |
| `processedFiles` | Pending files checked during deduplication |
| `matchedRanges` | Ranges accepted by Btrfs, including already shared ranges |
| `rejectedRanges` | Deduplication attempts rejected by Btrfs |
| `nextScanAt` | Planned next scan time while idle; zero in other phases |

Progress is published periodically as work advances, with a ten-second target
between updates. A slow filesystem operation or a large file can delay an update;
there is no reliable total-file count before discovery finishes, so the dashboard
uses actual counts and phase age instead of a synthetic percentage.

Read the activity together with `updatedAt`: a saved phase is not a liveness
check. The dashboard marks old active snapshots as stale instead of claiming
the daemon is still working. Older snapshots without activity remain readable.
Check the actual service and its recent progress with:

```sh
systemctl --user status project-bees.service
journalctl --user -u project-bees.service -n 20
```

Stopping during indexing preserves completed hash batches, but a restart
begins directory traversal again. Let an initial scan finish when possible;
repeated restarts delay its first deduplication stage.

## Scope and resource use

The index keeps 128 KiB content hashes and combines consecutive matches from
the same source into requests of at most 1 MiB. Btrfs verifies each request's
contents. If a combined range is rejected, the daemon retries its first
128 KiB chunk before trying another source. Gaps or a change in the selected
source end a combined range. Repeated chunks retain the same first source
selection as individual requests. Range counters count accepted requests;
byte counters continue to measure newly shared logical bytes.

During incremental passes, new content keys are grouped before matching old
files. Many identical new blocks therefore share one candidate lookup. The
set of new files is fixed for that lookup, so waking an old file does not
recursively wake other files. All existing index and cumulative statistics
remain compatible.

Unchanged files are recorded in a temporary integer-ID set during scanning.
Their persistent index rows are left untouched. A completed scan prunes
missing or ineligible files through the small file-ID index; an interrupted
scan keeps unseen rows for the next pass. This avoids rewriting every file's
metadata on each pass while preserving cached content hashes.

- The discovery pass walks accessible directories on mounted, writable
  Btrfs filesystems. It detects regular Git checkouts, linked worktrees,
  submodules and bare repositories, and gives nested repositories their own
  scope. Symlinks, hard-linked files and read-only subvolumes are skipped.
- Recognized build or dependency directories outside a concrete Git/artifact
  scope become their own scopes. Inside a Git repository, build and dependency
  files retain that repository's scope. Nested dependency directories stay in
  the outer artifact scope.
- Explicit cache/application containers and their immediate child directories
  become separate scopes. A recognized artifact or Git directory beneath a
  generic application scope can establish its own scope. Ordinary files outside
  the supported scopes remain excluded, as does the daemon's own state directory.
- Tracked, untracked and ignored regular files inside these scopes can
  participate, including Git metadata physically located there. Each match
  must span two distinct scope roots on the same Btrfs filesystem.
- The disk-backed SQLite index caches file metadata and hashes across restarts.
  SQLite's page-cache target is 8 MiB. The first pass hashes existing files.
  Later passes enumerate repository files and rehash new or changed files.
- Matching uses 128 KiB chunks aligned from each file's start, including
  eligible tails. Ranges smaller than a Btrfs sector, all-zero chunks, and
  matches shifted off these boundaries are skipped.
- `kernel_bytes` reports bytes accepted by the Btrfs ioctl, including regions
  that already shared storage. Check filesystem allocation to measure space
  reclaimed.

## Verify

Run `make test-project-bees` from a Btrfs checkout, or set
`PROJECT_BEES_TEST_DIR` to a writable Btrfs directory. The test verifies
physical extent sharing, content preservation, repository scope, and live
discovery of additions and edits. It also checks cumulative totals, restart
persistence, repeat-scan accounting, and statistics for non-UTF-8 repository
names, plus activity counters and phase transitions. It also covers combined
ranges and their size limit, mismatched offsets, sparse holes, hash-collision
fallback, incremental candidate selection, multilingual artifact discovery,
XDG application scopes, priority ordering and queue isolation, stable subtree
ownership, and bounded live event records. `make test` runs the upstream
unit tests. The GitHub workflow runs the Btrfs suite on an isolated loopback
filesystem.

To compare two executable versions on the same writable Btrfs filesystem:

```sh
python3 test/project-bees-benchmark.py --before /path/to/old/project-bees \
  --after bin/project-bees --directory /path/on/btrfs --repeats 3
```

The benchmark creates and removes its own temporary repositories and indexes.
It measures both a first pass over identical contiguous files and an
incremental pass with many repeated blocks. Each version receives independent
copies of the same data; content checks and shared-byte totals must agree.
JSON output includes per-run timings, kernel request counters, and medians.
Timings depend on the filesystem and host load, so they are reported without
a fixed pass/fail speed threshold.

The [upstream documentation](https://zygo.github.io/bees/) describes the
original filesystem-wide daemon. This fork retains the upstream source and
its GPL-3.0-or-later license. Upstream bees copyright 2015–2025 Zygo Blaxell.
