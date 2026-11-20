#include "crucible/city.h"
#include "crucible/fd.h"
#include "crucible/fs.h"
#include "crucible/openat2.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fts.h>
#include <getopt.h>
#include <iostream>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <map>
#include <memory>
#include <mntent.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/vfs.h>
#include <thread>
#include <vector>

namespace {

using crucible::Fd;
using std::string;
using Integer = sqlite3_int64;
namespace fs = std::filesystem;

constexpr size_t chunk_size = 128 * 1024;
volatile sig_atomic_t stopping = 0;

void stop(int)
{
	stopping = 1;
}

Fd open_path(const string &path, int flags, int dir = AT_FDCWD)
{
	open_how how {};
	how.flags = flags | O_CLOEXEC | O_NONBLOCK;
	how.resolve = RESOLVE_NO_SYMLINKS;
	return openat2(dir, path.c_str(), &how, sizeof(how));
}

bool is_entry(int dir, const char *name, mode_t type)
{
	struct stat st {};
	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
		(st.st_mode & S_IFMT) == type;
}

bool git_directory(int fd)
{
	return fd >= 0 && is_entry(fd, "HEAD", S_IFREG) &&
		is_entry(fd, "objects", S_IFDIR) &&
		(is_entry(fd, "refs", S_IFDIR) || is_entry(fd, "reftable", S_IFDIR));
}

bool git_repository(int fd)
{
	if (git_directory(fd)) {
		return true;
	}
	if (is_entry(fd, ".git", S_IFDIR)) {
		Fd metadata = open_path(".git", O_RDONLY | O_DIRECTORY, fd);
		return git_directory(metadata);
	}
	if (!is_entry(fd, ".git", S_IFREG)) {
		return false;
	}

	Fd gitfile = open_path(".git", O_RDONLY, fd);
	std::array<char, 4096> data;
	const auto size = pread(gitfile, data.data(), data.size(), 0);
	if (size <= 8 || size == static_cast<ssize_t>(data.size())) {
		return false;
	}

	const std::string_view line(data.data(), size);
	if (line.substr(0, 8) != "gitdir: " || line.find('\0') != line.npos) {
		return false;
	}
	const auto end = line.find_last_not_of("\r\n");
	Fd metadata = open_path(string(line.substr(8, end - 7)), O_RDONLY | O_DIRECTORY, fd);
	return git_directory(metadata) ||
		(static_cast<int>(metadata) >= 0 && is_entry(metadata, "HEAD", S_IFREG) &&
			is_entry(metadata, "commondir", S_IFREG));
}

string identity(const struct stat &st)
{
	return std::to_string(st.st_dev) + "/" + std::to_string(st.st_ino);
}

struct Repository {
	string path;
	string key;
	string fsid;
	size_t sector_size = 0;
};

using Mounts = std::map<string, bool>;

bool writable_btrfs(int fd)
{
	struct statfs info {};
	return fstatfs(fd, &info) == 0 && info.f_type == BTRFS_SUPER_MAGIC &&
		!(info.f_flags & ST_RDONLY);
}

Mounts read_mounts()
{
	std::unique_ptr<FILE, decltype(&endmntent)> mounts(setmntent("/proc/self/mounts", "r"), endmntent);
	if (!mounts) {
		throw std::runtime_error("cannot read /proc/self/mounts");
	}

	Mounts result;
	while (auto *mount = getmntent(mounts.get())) {
		result[mount->mnt_dir] = string(mount->mnt_type) == "btrfs" &&
			!hasmntopt(mount, "ro");
	}
	return result;
}

bool within(const string &path, const string &root)
{
	return path == root || root == "/" ||
		(path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

template <class VisitRepository, class VisitFile>
void walk(const std::vector<string> &requested, const string &state, VisitRepository found, VisitFile file)
{
	const auto mounts = read_mounts();
	std::vector<string> roots = requested;
	for (const auto &[path, eligible] : mounts) {
		if (eligible && (requested.empty() ||
			std::any_of(requested.begin(), requested.end(), [&](const auto &root) { return within(path, root); }))) {
			roots.push_back(path);
		}
	}
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

	for (auto &root : roots) {
		Fd root_fd = open_path(root, O_RDONLY | O_DIRECTORY);
		if (!writable_btrfs(root_fd) || stopping) {
			continue;
		}
		char *paths[] = {root.data(), nullptr};
		std::unique_ptr<FTS, decltype(&fts_close)> tree(fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, nullptr), fts_close);
		if (!tree) {
			throw std::runtime_error("cannot open directory walk for " + root);
		}
		struct Scope {
			std::shared_ptr<Repository> repo;
			bool metadata = false;
		};
		std::vector<Scope> scopes;
		while (!stopping) {
			errno = 0;
			auto *entry = fts_read(tree.get());
			if (!entry) {
				if (errno) {
					throw std::system_error(errno, std::generic_category(), "directory walk " + root);
				}
				break;
			}
			const size_t level = entry->fts_level;
			if (entry->fts_info == FTS_D) {
				scopes.resize(level);
				const string path = entry->fts_path;
				auto scope = level ? scopes.back() : Scope {};
				const bool boundary = level && mounts.count(path);
				if (boundary || within(path, state)) {
					fts_set(tree.get(), entry, FTS_SKIP);
					continue;
				}

				Fd dir;
				if (entry->fts_statp->st_ino == 256) {
					dir = open_path(path, O_RDONLY | O_DIRECTORY);
					uint64_t flags = 0;
					if (!writable_btrfs(dir) ||
						(ioctl(dir, BTRFS_IOC_SUBVOL_GETFLAGS, &flags) == 0 && (flags & BTRFS_SUBVOL_RDONLY))) {
						fts_set(tree.get(), entry, FTS_SKIP);
						continue;
					}
				}

				if (scope.repo && string(entry->fts_name) == ".git") {
					scope.metadata = true;
				}
				if (!scope.metadata) {
					if (!dir) {
						dir = open_path(path, O_RDONLY | O_DIRECTORY);
					}
					struct stat st {};
					if (git_repository(dir) && fstat(dir, &st) == 0) {
						crucible::BtrfsIoctlFsInfoArgs info;
						if (info.do_ioctl_nothrow(dir) && info.sectorsize && info.sectorsize <= chunk_size) {
							const auto uuid = info.fsid();
							scope.repo = std::make_shared<Repository>(Repository {
								path, identity(st), string(uuid.begin(), uuid.end()), info.sectorsize,
							});
							found(*scope.repo);
						}
					}
				}
				scopes.push_back(std::move(scope));
			} else if (entry->fts_info == FTS_F && level && scopes.size() >= level && scopes[level - 1].repo) {
				if (!mounts.count(entry->fts_path)) {
					file(*scopes[level - 1].repo, entry->fts_path, *entry->fts_statp);
				}
			}
		}
	}
}

void check(int code, sqlite3 *db)
{
	if (code != SQLITE_OK) {
		throw std::runtime_error(db ? sqlite3_errmsg(db) : sqlite3_errstr(code));
	}
}

class Statement {
	std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt {nullptr, sqlite3_finalize};
	sqlite3 *db;

public:
	Statement(sqlite3 *database, const char *sql) : db(database)
	{
		sqlite3_stmt *prepared = nullptr;
		check(sqlite3_prepare_v2(db, sql, -1, &prepared, nullptr), db);
		stmt.reset(prepared);
	}

	Statement &bind(int col, Integer value)
	{
		check(sqlite3_bind_int64(stmt.get(), col, value), db);
		return *this;
	}

	Statement &bind(int col, const string &value)
	{
		check(sqlite3_bind_blob(stmt.get(), col, value.data(), value.size(), SQLITE_TRANSIENT), db);
		return *this;
	}

	Integer number(int col) const { return sqlite3_column_int64(stmt.get(), col); }

	string bytes(int col) const
	{
		const auto *data = sqlite3_column_blob(stmt.get(), col);
		return data ? string(static_cast<const char *>(data), sqlite3_column_bytes(stmt.get(), col)) : string();
	}

	bool next()
	{
		const auto code = sqlite3_step(stmt.get());
		if (code == SQLITE_ROW) {
			return true;
		}
		if (code != SQLITE_DONE) {
			check(code, db);
		}
		return false;
	}

	void reset() { check(sqlite3_reset(stmt.get()), db); }

	void run()
	{
		next();
		reset();
	}
};

struct Stamp {
	// ctime detects same-size rewrites with a restored mtime and changes to permissions.
	std::array<Integer, 7> values;

	explicit Stamp(const struct stat &st) : values {
		static_cast<Integer>(st.st_dev), static_cast<Integer>(st.st_ino), st.st_size,
		st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_ctim.tv_sec, st.st_ctim.tv_nsec,
	} {}

	explicit Stamp(const Statement &row, int first)
	{
		for (size_t i = 0; i < values.size(); ++i) {
			values[i] = row.number(first + i);
		}
	}

	void bind(Statement &row, int first) const
	{
		for (size_t i = 0; i < values.size(); ++i) {
			row.bind(first + i, values[i]);
		}
	}
};

bool current(int fd, const Stamp &stamp)
{
	struct stat st {};
	return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
		Stamp(st).values == stamp.values;
}

Fd open_indexed(const string &path, const string &root, const string &repo, const Stamp &stamp)
{
	Fd dir = open_path(root, O_RDONLY | O_DIRECTORY);
	struct stat st {};
	if (fstat(dir, &st) < 0 || identity(st) != repo || !git_repository(dir)) {
		return {};
	}

	// Open the member relative to the verified repository directory.
	Fd fd = open_path(path.substr(root == "/" ? 1 : root.size() + 1), O_RDONLY, dir);
	return current(fd, stamp) && writable_btrfs(fd) ? fd : Fd {};
}

class Index {
	Fd lock;
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db {nullptr, sqlite3_close};

	void exec(const char *sql) { check(sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr), db.get()); }

public:
	explicit Index(const string &state)
	{
		lock = open((state + "/lock").c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
		if (!lock || flock(lock, LOCK_EX | LOCK_NB) < 0) {
			throw std::runtime_error("cannot lock index (is project-bees already running?): " + state);
		}
		sqlite3 *connection = nullptr;
		const int code = sqlite3_open_v2((state + "/index.sqlite3").c_str(), &connection,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, nullptr);
		db.reset(connection);
		check(code, db.get());
		check(sqlite3_busy_timeout(db.get(), 5000), db.get());
		exec("PRAGMA cache_size = -8192; PRAGMA temp_store = FILE; PRAGMA foreign_keys = ON;"
			"CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, path TEXT UNIQUE, root TEXT,"
			" repo TEXT, fsid TEXT, seen INTEGER, pending INTEGER, device INTEGER, inode INTEGER,"
			" size INTEGER, msec INTEGER, mnsec INTEGER, csec INTEGER, cnsec INTEGER);"
			"CREATE INDEX IF NOT EXISTS files_pending ON files (pending, id);"
			"CREATE TABLE IF NOT EXISTS chunks (file INTEGER REFERENCES files(id) ON DELETE CASCADE,"
			" start INTEGER, length INTEGER, hash INTEGER, PRIMARY KEY(file, start)) WITHOUT ROWID;"
			"CREATE INDEX IF NOT EXISTS chunk_match ON chunks (hash, length, file);");
	}

	void scan(const std::vector<string> &roots, const string &state)
	{
		auto *sql = db.get();
		Statement latest(sql, "SELECT coalesce(max(seen),0) + 1 FROM files");
		latest.next();
		const auto round = latest.number(0);
		latest.reset();

		Statement existing(sql, "SELECT id,root,repo,fsid,device,inode,size,msec,mnsec,csec,cnsec"
			" FROM files WHERE path=?");
		Statement touch(sql, "UPDATE files SET seen=? WHERE id=?");
		Statement remove(sql, "DELETE FROM files WHERE path=?");
		Statement add(sql, "INSERT INTO files(path,root,repo,fsid,seen,pending,device,inode,size,msec,mnsec,csec,cnsec)"
			" VALUES(?,?,?,?,?,1,?,?,?,?,?,?,?)");
		Statement add_chunk(sql, "INSERT INTO chunks VALUES (?,?,?,?)");

		std::array<char, chunk_size> buffer;
		const std::array<char, chunk_size> zeros {};
		size_t repositories = 0, changed = 0, indexed = 0, batch = 0;
		exec("BEGIN");
		walk(roots, state, [&](const Repository &) { ++repositories; },
			[&](const Repository &repo, const string &path, const struct stat &st) {
			if (st.st_nlink != 1 || !st.st_blocks || st.st_size < static_cast<off_t>(repo.sector_size)) {
				return;
			}
			if (++batch == 512) {
				exec("COMMIT; BEGIN");
				batch = 0;
			}
			const Stamp stamp(st);
			existing.bind(1, path);
			const bool unchanged = existing.next() && existing.bytes(1) == repo.path &&
				existing.bytes(2) == repo.key && existing.bytes(3) == repo.fsid &&
				stamp.values == Stamp(existing, 4).values;
			if (unchanged) {
				touch.bind(1, round).bind(2, existing.number(0)).run();
			}
			existing.reset();
			if (unchanged) {
				return;
			}

			Fd fd = open_path(path, O_RDONLY);
			if (!current(fd, stamp)) {
				return;
			}
			remove.bind(1, path).run();
			add.bind(1, path).bind(2, repo.path).bind(3, repo.key).bind(4, repo.fsid).bind(5, round);
			stamp.bind(add, 6);
			add.run();
			const auto id = sqlite3_last_insert_rowid(sql);
			bool complete = true;
			for (off_t offset = 0; offset < st.st_size;) {
				// SEEK_DATA avoids reading large sparse holes. Align back to the
				// chunk boundary so a partially populated chunk is indexed too.
				const auto data = lseek(fd, offset, SEEK_DATA);
				if (data < 0 && errno == ENXIO) {
					break;
				}
				if (data > offset) {
					offset = data - data % chunk_size;
				}
				const auto len = std::min<off_t>(chunk_size, st.st_size - offset);
				if (len < static_cast<off_t>(repo.sector_size)) {
					break;
				}
				if (stopping || pread(fd, buffer.data(), len, offset) != len) {
					complete = false;
					break;
				}
				if (std::memcmp(buffer.data(), zeros.data(), len)) {
					const auto hash = CityHash64(buffer.data(), len);
					add_chunk.bind(1, id).bind(2, offset).bind(3, len).bind(4, hash).run();
					++indexed;
				}
				offset += len;
			}
			if (!complete || !current(fd, stamp)) {
				remove.bind(1, path).run();
			} else {
				++changed;
			}
		});
		if (!stopping) {
			Statement prune(sql, "DELETE FROM files WHERE seen<>?");
			prune.bind(1, round).run();
		}
		exec("COMMIT");
		std::cout << "scan repositories=" << repositories << " hashed_files=" << changed
			<< " indexed_chunks=" << indexed << (stopping ? " interrupted" : "") << std::endl;
	}

	void dedup()
	{
		auto *sql = db.get();
		Statement has_existing(sql, "SELECT 1 FROM files WHERE pending=0 LIMIT 1");
		const bool have_existing = has_existing.next();
		has_existing.reset();
		if (have_existing) {
			// A new repository can make several existing files eligible for
			// the first time, even if those files have not changed.
			exec("UPDATE files SET pending=1 WHERE id IN ("
				" SELECT old.id FROM files fresh JOIN chunks added ON added.file=fresh.id"
				" JOIN chunks match ON match.hash=added.hash AND match.length=added.length"
				" JOIN files old ON old.id=match.file"
				" WHERE fresh.pending=1 AND old.pending=0 AND fresh.fsid=old.fsid AND fresh.repo<>old.repo)");
		}
		Statement pending(sql, "SELECT id,path,root,repo,fsid,device,inode,size,msec,mnsec,csec,cnsec"
			" FROM files WHERE pending=1 AND id>? ORDER BY id LIMIT 1");
		Statement chunks(sql, "SELECT start,length,hash FROM chunks WHERE file=?");
		Statement sources(sql, "SELECT f.id,f.path,f.root,f.repo,f.device,f.inode,f.size,"
			" f.msec,f.mnsec,f.csec,f.cnsec,c.start FROM chunks c JOIN files f ON f.id=c.file"
			" WHERE c.hash=? AND c.length=? AND f.fsid=? AND f.repo<>? ORDER BY c.file");
		Statement done(sql, "UPDATE files SET pending=0 WHERE id=?");

		Integer id = 0;
		uint64_t bytes = 0, ranges = 0, rejected = 0;
		int last_error = 0;
		unsigned batch = 0;
		exec("BEGIN");
		while (!stopping && pending.bind(1, id).next()) {
			id = pending.number(0);
			const auto root = pending.bytes(2);
			const auto repo = pending.bytes(3);
			const auto fsid = pending.bytes(4);
			const Fd dest = open_indexed(pending.bytes(1), root, repo, Stamp(pending, 5));
			pending.reset();
			bool retry = false;
			if (static_cast<int>(dest) >= 0) {
				chunks.bind(1, id);
				Fd source;
				Integer source_id = 0;
				while (!stopping && chunks.next()) {
					const auto offset = chunks.number(0);
					const auto length = chunks.number(1);
					sources.bind(1, chunks.number(2)).bind(2, length).bind(3, fsid).bind(4, repo);
					while (!stopping && sources.next()) {
						if (source_id != sources.number(0)) {
							source_id = sources.number(0);
							source = open_indexed(sources.bytes(1), sources.bytes(2),
								sources.bytes(3), Stamp(sources, 4));
						}
						if (!source) {
							continue;
						}
						crucible::BtrfsExtentSame same(source, sources.number(11), length);
						same.add(dest, offset);
						int status = 0;
						try {
							same.do_ioctl();
							status = same.m_info.front().status;
						} catch (const std::system_error &error) {
							status = -error.code().value();
						}
						if (status == 0) {
							bytes += same.m_info.front().bytes_deduped;
							++ranges;
							break;
						}
						++rejected;
						last_error = status;
						if (status == -ENOSPC || status == -ENOMEM || status == -EAGAIN || status == -EINTR) {
							retry = true;
							break;
						}
					}
					sources.reset();
					if (retry) {
						break;
					}
				}
				chunks.reset();
			}
			if (!retry && !stopping) {
				done.bind(1, id).run();
			}
			if (++batch == 512) {
				exec("COMMIT; BEGIN");
				batch = 0;
			}
		}
		pending.reset();
		exec("COMMIT");
		std::cout << "dedup ranges=" << ranges << " kernel_bytes=" << bytes
			<< " rejected=" << rejected << " last_status=" << last_error
			<< (stopping ? " interrupted" : "") << std::endl;
	}
};

string state_directory()
{
	const auto *state = std::getenv("XDG_STATE_HOME");
	if (state && *state) {
		return string(state) + "/project-bees";
	}
	const auto *home = std::getenv("HOME");
	if (!home || !*home) {
		throw std::runtime_error("set HOME, XDG_STATE_HOME or --state-dir");
	}
	return string(home) + "/.local/state/project-bees";
}

} // namespace

int main(int argc, char **argv)
try {
	std::vector<string> roots;
	string state;
	bool once = false, discover = false;
	long interval = 300;
	const option options[] = {
		{"root", required_argument, nullptr, 'r'},
		{"state-dir", required_argument, nullptr, 's'},
		{"interval", required_argument, nullptr, 'i'},
		{"once", no_argument, nullptr, '1'},
		{"discover", no_argument, nullptr, 'd'},
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0},
	};
	for (int opt; (opt = getopt_long(argc, argv, "r:s:i:1dh", options, nullptr)) != -1;) {
		switch (opt) {
		case 'r':
			roots.push_back(fs::canonical(optarg).string());
			if (!fs::is_directory(roots.back())) {
				throw std::runtime_error("--root expects a directory");
			}
			break;
		case 's': state = optarg; break;
		case 'i': {
			char *end = nullptr;
			interval = std::strtol(optarg, &end, 10);
			if (end == optarg || *end || interval < 1 || interval > 86400) {
				throw std::runtime_error("--interval expects 1 to 86400 seconds");
			}
			break;
		}
		case '1': once = true; break;
		case 'd': discover = true; break;
		case 'h':
			std::cout << "Usage: project-bees [--root DIR]... [--state-dir DIR]\n"
				"                    [--interval SECONDS] [--once | --discover]\n"
				"Discover Git repositories on mounted writable Btrfs filesystems and deduplicate\n"
				"ranges across repositories. The default interval is 300 seconds after each pass.\n";
			return 0;
		default: return 2;
		}
	}
	if (optind != argc || (once && discover)) {
		throw std::runtime_error("invalid arguments; see --help");
	}
	std::signal(SIGINT, stop);
	std::signal(SIGTERM, stop);
	umask(0077);
	state = fs::weakly_canonical(fs::absolute(state.empty() ? state_directory() : state)).string();
	if (discover) {
		walk(roots, state, [](const Repository &repo) { std::cout << repo.path << '\n'; },
			[](const Repository &, const string &, const struct stat &) {});
		return 0;
	}
	fs::create_directories(state);
	Index index(state);
	std::cout << "project-bees running, scan interval=" << interval << "s" << std::endl;
	while (!stopping) {
		index.scan(roots, state);
		if (!stopping) {
			index.dedup();
		}
		if (once) {
			break;
		}
		for (long seconds = 0; seconds < interval && !stopping; ++seconds) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
	return 0;
} catch (const std::exception &error) {
	std::cerr << "project-bees: " << error.what() << '\n';
	return 1;
}
