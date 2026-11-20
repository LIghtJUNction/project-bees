# project-bees

`project-bees` is a fork of [Zygo/bees](https://github.com/Zygo/bees) that
shares identical Btrfs file ranges across local Git repositories. It runs as a
regular user and uses the upstream CityHash and Btrfs extent-same code. Btrfs
compares the data in the kernel before sharing storage. Subsequent writes use
copy-on-write.

## Build and run

Build dependencies are GNU make, a C++17 compiler, and SQLite development
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
discovery of additions and edits. `make test` runs the upstream unit tests.

The [upstream documentation](https://zygo.github.io/bees/) describes the
original filesystem-wide daemon. This fork retains the upstream source and
its GPL-3.0-or-later license. Upstream bees copyright 2015–2025 Zygo Blaxell.
