# project-bees

`project-bees` is a fork of [Zygo/bees](https://github.com/Zygo/bees) that
shares identical Btrfs file ranges across local Git repositories. It runs as a
regular user and uses the upstream CityHash and Btrfs extent-same code. Btrfs
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

`--discover` lists the discovered repository roots without creating an index.
`--once` performs one scan and deduplication pass. With neither flag the daemon
waits five minutes after each pass, then picks up new repositories, changed
files, and deletions. Use `--interval SECONDS` to change that delay, or repeat
`--root DIR` to limit where it looks for repositories.

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

## Scope and resource use

- The discovery pass walks accessible directories on mounted, writable
  Btrfs filesystems. It detects regular Git checkouts, linked worktrees,
  submodules and bare repositories, and gives nested repositories their own
  scope. Symlinks, hard-linked files and read-only subvolumes are skipped.
- Tracked, untracked and ignored regular files inside those repositories can
  participate, including Git metadata physically located there. Each match
  must span two distinct repository roots on the same Btrfs filesystem.
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
names. `make test` runs the upstream unit tests.

The [upstream documentation](https://zygo.github.io/bees/) describes the
original filesystem-wide daemon. This fork retains the upstream source and
its GPL-3.0-or-later license. Upstream bees copyright 2015–2025 Zygo Blaxell.
