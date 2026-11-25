# project-bees

`project-bees` is a fork of [Zygo/bees](https://github.com/Zygo/bees) that
shares identical Btrfs file ranges across local Git and README-marked projects,
language build and dependency directories, package caches, backups, and application data.
It runs as a regular user and uses the
upstream CityHash and Btrfs extent-same code. Btrfs
compares the data in the kernel before sharing storage. Subsequent writes use
copy-on-write.

## How block sharing works

The index hashes aligned 128 KiB chunks to find candidate matches. It can merge
consecutive matching chunks into a request of up to 1 MiB. A matching hash is
only a candidate: the Btrfs deduplication ioctl compares the actual bytes before
sharing the physical range. Files keep their names and contents, and later
writes use copy-on-write. When FIEMAP shows that both ranges already map to
the same uncompressed blocks, the daemon skips the redundant deduplication
request. The log reports these matches as `already_shared_ranges`. The
mechanism follows the upstream
[Btrfs deduplication interface](https://btrfs.readthedocs.io/en/latest/Deduplication.html).

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
omarchy plugin add https://github.com/LIghtJUNction/omarchy-project-bees.git --enable
```

Click its bar widget or run
`omarchy-shell shell summon io.github.lightjunction.project-bees` to open it.
The dashboard also shows whether the latest snapshot reports indexing,
deduplication, a wait before the next scan, or a stopped daemon. With plugin
0.1.4, its progress area shows the priority/full pass, current priority directory,
file and chunk counts, and phase age. Plugin 0.1.5 also shows how many
already-shared ranges the daemon skipped when running project-bees 0.1.7 or
later. The live log immediately shows the current
activity, retains up to 32 observed progress changes, and combines them with up to
32 actual file-to-file sharing records. Progress and positive newly shared bytes
are labeled separately.
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

### Scan order and recognized scopes

Default system scans process likely duplicate locations before a complete
discovery pass: managed tool installations, local tools, Flatpak application
data, `/var/tmp`, common language caches, application and backup containers,
and the XDG cache, data, configuration and state directories.
Each priority directory is scanned and deduplicated before the
next one. This order is a heuristic based on where repeated builds and installed
dependencies accumulate; contents still determine whether a match is valid.

| Area | Discovery examples |
| --- | --- |
| JavaScript / TypeScript | `node_modules`, `.yarn`, `.next`, `.nuxt`, `.svelte-kit`, `.parcel-cache`, `.turbo`; npm, Yarn, pnpm and Bun caches |
| Rust / JVM | `target`, `.gradle`; Cargo registry/Git, Rustup toolchains, Maven, Gradle, Ivy and Coursier caches |
| Python | `.venv`, `venv`, `site-packages`, `__pycache__`, `.tox`, `.nox`; pip, uv and Poetry caches |
| C/C++ / .NET | CMake build trees; manifest-qualified `build`, `bin`, `obj`, `out`, `artifacts`; ccache, sccache and NuGet caches |
| Other language ecosystems | `.dart_tool`, `Pods`, `.build`, Zig caches/output, `_build`, `.stack-work`, `dist-newstyle`, `_opam`; Composer, Bundler, Go, Pub and other package caches |
| Managed tool installations | mise/asdf `installs` and `downloads`, SDKMAN `candidates`, nvm Node versions, pyenv/rbenv versions, uv Python installations |
| Standalone user tools | Direct regular files in `~/.local/bin`, `${CARGO_HOME:-~/.cargo}/bin`, and `${XDG_DATA_HOME:-~/.local/share}/claude/versions` |
| Application and backup containers | `/opt`, `~/opt`, `~/.local/opt`, `/var/backups`, `~/Backups`, `~/backups`, `~/备份` |
| User application files | `${XDG_CACHE_HOME:-~/.cache}`, `${XDG_DATA_HOME:-~/.local/share}`, `${XDG_CONFIG_HOME:-~/.config}`, `${XDG_STATE_HOME:-~/.local/state}` |
| Flatpak application files | `~/.var/app/<app-id>`, including the application's configuration, data and cache directories |

A directory with a regular `README.md` file is recognized even without `.git`;
the README name is matched without ASCII case sensitivity. Removing `.git`
while retaining the README preserves the project identity and cached hashes.
Nested README files keep the existing parent project owner. An arbitrary
directory containing `pyvenv.cfg` or `CMakeCache.txt` is also recognized. Generic names such as `build`, `dist`, `bin`, `obj`, `vendor` and
`deps` require a supported project manifest in that directory or its parent.
This avoids treating every system `bin` or unrelated `build` directory as a
project output tree. Relevant absolute cache environment variables are honored;
relative XDG paths fall back to the default user directories.

Known installation layouts are split at the version boundary: for example,
`mise/installs/node/24.0.0` and `mise/installs/node/24.1.0` are separate scopes.
This also works when a version manager is itself a Git checkout. Nested build
and dependency directories stay with their installed version; nested Git
repositories and recognized backups keep their own identities. Hidden manager
entries do not create version scopes. The rules match the configured manager
paths, not every directory named `versions` or `installs`.

Absolute `MISE_DATA_DIR`, `MISE_INSTALLS_DIR`, `MISE_DOWNLOADS_DIR`,
`UV_PYTHON_INSTALL_DIR`, `NVM_DIR`, `PYENV_ROOT`, `RBENV_ROOT`, `SDKMAN_DIR`
and `ASDF_DATA_DIR` overrides are supported. The service must receive these
variables in its own environment to use custom paths. The managed layouts
follow the [mise directories](https://mise.jdx.dev/directories.html),
[uv storage](https://docs.astral.sh/uv/reference/storage/), and
[nvm installation](https://github.com/nvm-sh/nvm#installing-and-updating)
conventions. Flatpak's per-application paths follow its
[XDG directory conventions](https://docs.flatpak.org/en/latest/conventions.html#xdg-base-directories).

Direct tool files in the listed executable and Claude version directories
are individual scopes, so separate binaries can share matching ranges. Symbolic
links and files with more than one hard link retain the usual exclusions.

Backup names are matched without ASCII case sensitivity. File suffixes include
`.bak`, `.backup`, `.old`, `.orig`, `.save`, and `~`. The tokens `backup`,
`backups`, `bak`, and `备份` also match standalone names or prefixes/suffixes
separated by a dot, hyphen, underscore, or space. Common date suffixes are
accepted, such as `project.BAK-20261121` and `file.bak.2026-11-21_123045`.
Bare names such as `old`, `copy`, `bakery`, or a date alone do not create scopes.

Backup files and directories have distinct identities, including inside a Git
or build directory, so a file can share storage with its adjacent backup.
Outside recognized projects, only an existing original sibling that can be
inferred from a backup name is included: `foo.txt.bak` can include `foo.txt`,
and `project.bak` can include `project/`. The precise original name takes
precedence over stripping a date. A backup does not turn its whole parent
directory into a scan scope. Backups remain intact; discovery never deletes them.

Only Btrfs files are eligible. Files on tmpfs, including a tmpfs-mounted `/tmp`,
do not participate. The kernel enforces permission to deduplicate each destination;
files owned by the service's user can participate even when their mode is read-only.
Recognizing a system directory such as `/opt` does not grant access or change its
permissions. Read-only Btrfs mounts and subvolumes remain excluded.

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
Source files must remain inside any current explicit root limits. When managed
installation rules change an old scope, partial scans recheck its current owner
before using it, so old and new index entries cannot make one version appear
to be two independent scopes.
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
changes and checks again every 10 seconds.

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
| `projects[].path` | Scope path: project, build/cache directory, storage container, or recognized backup-related file |
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
| `matchedRanges` | Matching ranges accepted by Btrfs or confirmed to already share the same blocks |
| `alreadySharedRanges` | Subset of matches that already shared the same blocks and skipped a redundant deduplication request |
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

Within a deduplication pass, a bounded cache retains up to 64 first candidate
query results for repeated content keys in the same destination scope and
filesystem. It preserves source ordering and is discarded before the next pass.
A cache hit preserves the existing file validation and Btrfs byte comparison.
An invalid or mismatching first candidate falls back to the remaining sources.
The cache stores neither file descriptors nor a conclusion that two files match.

Unchanged files are recorded in a temporary integer-ID set during scanning.
Their persistent index rows are left untouched. A completed scan prunes
missing or ineligible files through the small file-ID index; an interrupted
scan keeps unseen rows for the next pass. This avoids rewriting every file's
metadata on each pass while preserving cached content hashes.
If only a file's scope changes while its filesystem and complete metadata
stamp remain unchanged, the daemon revalidates the file and updates its owner
without reading its contents again. Its existing chunks stay indexed, and it
is marked pending so newly possible cross-scope matches are processed.

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

## Publish a GitHub release

In GitHub Actions, open **Release Project Bees**, select `main` and choose a
`patch`, `minor`, or `major` bump. The default run shows the next version and
source commit, then runs the Btrfs tests. Select **Create the Git tag and publish a
GitHub Release** when you want to publish after the tests pass.

Project Bees takes its release version from the highest stable `vMAJOR.MINOR.PATCH`
Git tag. A release creates the next tag on the tested commit and publishes
generated notes and GitHub's source archives. If a run fails after creating its
tag, choose **Re-run failed jobs** on that same Actions run. It can finish the
original release while the tagged commit is still in `main`'s history. If the
original run left no tag, start a new publish run. A new publish run stops if an
earlier run left an unfinished release.

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
ownership, managed versions and standalone tool files, legacy scope migration,
first-candidate cache fallback, scope changes without rehashing,
and bounded live event records. `make test` runs the upstream
unit tests. The GitHub workflow runs the Btrfs suite on an isolated loopback
filesystem.

To compare two executable versions on the same writable Btrfs filesystem:

```sh
python3 test/project-bees-benchmark.py --before /path/to/old/project-bees \
  --after bin/project-bees --directory /path/on/btrfs --repeats 3
```

The benchmark creates and removes its own temporary repositories and indexes.
It measures a first pass over identical contiguous files and an
incremental pass with many repeated blocks, plus a first pass with no matching
content to expose lookup overhead when there is nothing to share. Each version
receives independent copies of the same data; content checks and shared-byte
totals must agree.
JSON output includes per-run timings, kernel request counters, and medians.
Timings depend on the filesystem and host load, so they are reported without
a fixed pass/fail speed threshold.

The [upstream documentation](https://zygo.github.io/bees/) describes the
original filesystem-wide daemon. This fork retains the upstream source and
its GPL-3.0-or-later license. Upstream bees copyright 2015–2025 Zygo Blaxell.

### Pause and resume scanning

```sh
project-bees --pause
project-bees --status
project-bees --resume
```

Use the daemon's `--state-dir` for each command if customized. Commands are
idempotent; `--pause` also persists across daemon restarts. `--resume` clears
both manual and automatic requests. `--status` reports `running`, `requested`,
`paused` (acknowledged by the live daemon), `manual`, and `automatic` separately.
A resume request may briefly show `requested=0 paused=1` until acknowledged.
These commands do not start or stop the systemd service. Existing launch options
and `--stats` JSON remain compatible.

Pause is cooperative: the daemon commits its current batch between directory
entries or complete files before acknowledging. It retains traversal and index
state in the same process and continues on resume. Large files, directory reads,
hashing, SQLite operations and deduplication syscalls can delay acknowledgement;
there is no immediate-response guarantee for a blocked syscall. A paused daemon
checks requests once per second and accepts normal SIGINT/SIGTERM shutdown.
The index lock stays held while paused; `running=0` means any old acknowledgement
is ignored. Discovery-only mode does not use daemon pause control.

For optional automatic pauses, add `--auto-pause` to your existing daemon command
(or your own systemd ExecStart override). It is **off by default**. The daemon
samples Linux `/proc/pressure/memory` `some avg10` at most once per second at safe
boundaries. At least ten continuous seconds at 20% or above latch an automatic
pause. Resume manually with `--resume`; pressure dropping never resumes work.
The trigger rearms only after a valid sample at 10% or below, so sustained high
pressure does not repeatedly pause after a manual resume. Missing/invalid PSI
and sampling gaps longer than two seconds break the sustained window; missing
samples neither resume nor rearm. Platforms without PSI keep manual control and
skip automatic triggers. Memory mode measures memory stall pressure, not general UI
responsiveness; I/O stall monitoring is separately available below. No system resource settings are changed.

Unprivileged control tests (use a temporary directory, no Btrfs required):

```sh
make test-project-bees-control
```

For I/O stalls, enable `--auto-pause-io` independently of memory PSI. It reads
Linux `/proc/pressure/io` **`full avg60`**, so prolonged I/O stalls can pause work
even when memory remains available. Both sources use the same 20% high / 10% low
thresholds and ten-second continuous observation window, with separate timers
and rearming state. `avg60` is a kernel rolling average; the ten-second window
is additional debounce, not a claim of instant stall detection. A short high
sample does not trigger. Low or unavailable readings never release a latched
pause; `--resume` clears both source requests and any manual request.

`--auto-pause-memory` is an explicit alias for the existing `--auto-pause`
(memory-only, unchanged). To enable both, use:

```sh
project-bees --auto-pause-memory --auto-pause-io
```

Both remain off unless selected. An unavailable/invalid source only interrupts
its own observation window; the other source continues independently. Status
adds `automatic_memory` and `automatic_io` to identify latched sources while
retaining the combined `automatic` field. A low reading rearms only its own
source. These are host PSI signals and cannot prove Project Bees caused the
stalls. Safe-boundary and blocked-syscall response limits still apply.
