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
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <fts.h>
#include <getopt.h>
#include <iostream>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <map>
#include <memory>
#include <mntent.h>
#include <set>
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
constexpr Integer max_dedup_size = 1024 * 1024;
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

bool build_manifest(int dir)
{
	for (const auto *name : {"Makefile", "package.json", "pom.xml", "build.gradle",
		"build.gradle.kts", "pubspec.yaml", "CMakeLists.txt", "build.zig", "meson.build",
		"Cargo.toml", "pyproject.toml", "setup.py", "composer.json", "go.mod", "Gemfile",
		"Package.swift", "mix.exs", "rebar.config"}) {
		if (is_entry(dir, name, S_IFREG)) {
			return true;
		}
	}
	const int copy = fcntl(dir, F_DUPFD_CLOEXEC, 0);
	if (copy < 0) {
		return false;
	}
	const auto close_directory = [](DIR *value) { closedir(value); };
	std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(copy), close_directory);
	if (!entries) {
		close(copy);
		return false;
	}
	while (const auto *entry = readdir(entries.get())) {
		const std::string_view name(entry->d_name);
		const auto dot = name.find_last_of('.');
		if (dot != name.npos) {
			const auto suffix = name.substr(dot);
			if ((suffix == ".csproj" || suffix == ".fsproj" || suffix == ".vbproj" || suffix == ".vcxproj") &&
				is_entry(dir, entry->d_name, S_IFREG)) {
				return true;
			}
		}
	}
	return false;
}

bool artifact_directory(std::string_view name, int dir)
{
	for (const auto *candidate : {"target", "node_modules", "bower_components",
		".venv", "venv", "site-packages", "__pycache__", ".tox", ".nox", ".mypy_cache",
		".pytest_cache", ".ruff_cache", ".next", ".nuxt", ".svelte-kit", ".angular",
		".parcel-cache", ".turbo", ".yarn", ".gradle", ".dart_tool", "Pods", ".build", "_build",
		".zig-cache", "zig-cache", "zig-out", "zig-pkg",
		".stack-work", "dist-newstyle", "_opam"}) {
		if (name == candidate) {
			return true;
		}
	}
	if (is_entry(dir, "pyvenv.cfg", S_IFREG) || is_entry(dir, "CMakeCache.txt", S_IFREG)) {
		return true;
	}
	for (const auto *candidate : {"build", "dist", "bin", "obj", "out", "artifacts", "vendor", "deps"}) {
		if (name == candidate) {
			if (build_manifest(dir)) {
				return true;
			}
			Fd parent = open_path("..", O_RDONLY | O_DIRECTORY, dir);
			return static_cast<int>(parent) >= 0 && build_manifest(parent);
		}
	}
	return false;
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

string absolute_directory(const string &path)
{
	if (path.empty() || !fs::path(path).is_absolute()) {
		return {};
	}
	auto result = fs::path(path).lexically_normal().string();
	while (result.size() > 1 && result.back() == '/') {
		result.pop_back();
	}
	return result;
}

string environment_directory(const char *name, const string &fallback)
{
	const auto *value = std::getenv(name);
	const auto configured = value ? absolute_directory(value) : string();
	return configured.empty() ? fallback : configured;
}

string child_directory(const string &parent, const char *child)
{
	return parent.empty() ? string() : (fs::path(parent) / child).string();
}

class ScanPolicy {
	std::vector<string> priority;
	std::set<string, std::less<>> storage;

	void add(const string &path)
	{
		auto normalized = absolute_directory(path);
		if (!normalized.empty() && storage.insert(normalized).second) {
			priority.push_back(std::move(normalized));
		}
	}

public:
	explicit ScanPolicy(const std::vector<string> &extra_priority_roots = {})
	{
		for (const auto &path : extra_priority_roots) {
			add(fs::absolute(path).string());
		}
		const auto home = environment_directory("HOME", {});
		const auto cache = environment_directory("XDG_CACHE_HOME", child_directory(home, ".cache"));
		const auto data = environment_directory("XDG_DATA_HOME", child_directory(home, ".local/share"));
		const auto config = environment_directory("XDG_CONFIG_HOME", child_directory(home, ".config"));
		const auto state = environment_directory("XDG_STATE_HOME", child_directory(home, ".local/state"));
		const auto cargo = environment_directory("CARGO_HOME", child_directory(home, ".cargo"));
		const auto rustup = environment_directory("RUSTUP_HOME", child_directory(home, ".rustup"));
		const auto gradle = environment_directory("GRADLE_USER_HOME", child_directory(home, ".gradle"));
		const auto bundle = environment_directory("BUNDLE_USER_HOME", child_directory(home, ".bundle"));
		add("/var/tmp");
		for (const auto &path : {
			child_directory(cargo, "registry"), child_directory(cargo, "git"),
			child_directory(rustup, "toolchains"),
			environment_directory("GOMODCACHE", child_directory(home, "go/pkg/mod")),
			environment_directory("GOCACHE", child_directory(cache, "go-build")),
			child_directory(home, ".m2/repository"), child_directory(gradle, "caches"),
			child_directory(home, ".ivy2/cache"), child_directory(cache, "coursier"),
			environment_directory("NUGET_PACKAGES", child_directory(home, ".nuget/packages")),
			environment_directory("NUGET_HTTP_CACHE_PATH", child_directory(data, "NuGet/v3-cache")),
			environment_directory("UV_CACHE_DIR", child_directory(cache, "uv")),
			environment_directory("PIP_CACHE_DIR", child_directory(cache, "pip")),
			child_directory(cache, "pypoetry"),
			environment_directory("npm_config_cache", child_directory(home, ".npm")),
			environment_directory("YARN_CACHE_FOLDER", child_directory(cache, "yarn")),
			child_directory(home, ".yarn/berry"),
			child_directory(environment_directory("PNPM_HOME", child_directory(data, "pnpm")), "store"),
			child_directory(home, ".bun/install/cache"),
			environment_directory("PUB_CACHE", child_directory(home, ".pub-cache")),
			environment_directory("COMPOSER_CACHE_DIR", child_directory(cache, "composer")),
			environment_directory("BUNDLE_USER_CACHE", child_directory(bundle, "cache")),
			child_directory(home, ".gem"), child_directory(home, ".cabal/store"),
			child_directory(home, ".stack"), child_directory(home, ".julia"),
			environment_directory("OPAMROOT", child_directory(home, ".opam")),
			child_directory(home, ".hex"), child_directory(home, ".mix"),
			environment_directory("CCACHE_DIR", child_directory(cache, "ccache")),
			environment_directory("SCCACHE_DIR", child_directory(cache, "sccache")),
			child_directory(home, ".conan2/p"), child_directory(cache, "vcpkg"),
			child_directory(cache, "org.swift.swiftpm")}) {
			add(path);
		}
		add(cache);
		add(data);
		add(config);
		add(state);
	}

	std::vector<string> priority_roots(const std::vector<string> &requested) const
	{
		std::vector<string> result;
		for (const auto &path : priority) {
			if (!requested.empty() && std::none_of(requested.begin(), requested.end(),
				[&](const auto &root) { return within(path, root); })) {
				continue;
			}
			const Fd dir = open_path(path, O_RDONLY | O_DIRECTORY);
			if (writable_btrfs(dir)) {
				result.push_back(path);
			}
		}
		return result;
	}

	bool storage_scope(const string &path) const
	{
		const std::string_view view(path);
		const auto slash = view.find_last_of('/');
		const auto parent = slash == view.npos ? std::string_view() : view.substr(0, slash ? slash : 1);
		return storage.count(view) || storage.count(parent);
	}
};

std::vector<string> traversal_roots(const std::vector<string> &requested, const Mounts &mounts)
{
	std::vector<string> roots = requested;
	for (const auto &[path, eligible] : mounts) {
		if (eligible && (requested.empty() ||
			std::any_of(requested.begin(), requested.end(), [&](const auto &root) { return within(path, root); }))) {
			roots.push_back(path);
		}
	}
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
	return roots;
}

struct WalkScope {
	std::shared_ptr<Repository> repo;
	bool metadata = false;
	bool storage = false;
};

bool readable_scope_directory(int fd, struct stat &st)
{
	if (fstat(fd, &st) < 0) {
		return false;
	}
	if (st.st_ino == BTRFS_FIRST_FREE_OBJECTID) {
		uint64_t flags = 0;
		return writable_btrfs(fd) && ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags) == 0 &&
			!(flags & BTRFS_SUBVOL_RDONLY);
	}
	return true;
}

void select_scope(const string &path, int dir, const struct stat &st, const ScanPolicy &policy, WalkScope &scope)
{
	const auto name = std::string_view(path).substr(path.find_last_of('/') + 1);
	if (scope.repo && name == ".git") {
		scope.metadata = true;
	}
	if (scope.metadata) {
		return;
	}
	const bool git = git_repository(dir);
	const bool artifact = !git && (!scope.repo || scope.storage) && artifact_directory(name, dir);
	const bool storage = !git && !artifact && (!scope.repo || scope.storage) && policy.storage_scope(path);
	if (git || artifact || storage) {
		crucible::BtrfsIoctlFsInfoArgs info;
		if (info.do_ioctl_nothrow(dir) && info.sectorsize && info.sectorsize <= chunk_size) {
			const auto uuid = info.fsid();
			scope.repo = std::make_shared<Repository>(Repository {
				path, identity(st), string(uuid.begin(), uuid.end()), info.sectorsize,
			});
			scope.storage = storage;
		}
	}
}

bool inherit_scope(const string &root, const string &state, const ScanPolicy &policy,
	const Mounts &mounts, const std::vector<string> &boundaries, WalkScope &scope)
{
	// Enter a priority subtree with the same owner as the complete traversal.
	string boundary;
	for (const auto &path : boundaries) {
		if (within(root, path) && path.size() > boundary.size()) {
			boundary = path;
		}
	}
	if (boundary.empty()) {
		return false;
	}
	std::vector<string> ancestors;
	for (auto path = fs::path(root); path.string() != boundary;) {
		path = path.parent_path();
		ancestors.push_back(path.string());
	}
	std::reverse(ancestors.begin(), ancestors.end());
	for (const auto &path : ancestors) {
		if (within(path, state) || (path != boundary && mounts.count(path))) {
			return false;
		}
		const Fd dir = open_path(path, O_RDONLY | O_DIRECTORY);
		struct stat st {};
		if (!readable_scope_directory(dir, st) || (path == boundary && !writable_btrfs(dir))) {
			return false;
		}
		select_scope(path, dir, st, policy, scope);
	}
	return true;
}

template <class VisitRepository, class VisitFile>
void walk(const std::vector<string> &requested, const string &state, const ScanPolicy &policy,
	VisitRepository found, VisitFile file, const std::vector<string> *complete_roots = nullptr)
{
	const auto mounts = read_mounts();
	auto roots = traversal_roots(requested, mounts);
	const auto boundaries = complete_roots ? traversal_roots(*complete_roots, mounts) : std::vector<string>();

	for (auto &root : roots) {
		Fd root_fd = open_path(root, O_RDONLY | O_DIRECTORY);
		if (!writable_btrfs(root_fd) || stopping) {
			continue;
		}
		WalkScope initial;
		if (complete_roots && !inherit_scope(root, state, policy, mounts, boundaries, initial)) {
			continue;
		}
		char *paths[] = {root.data(), nullptr};
		std::unique_ptr<FTS, decltype(&fts_close)> tree(
			fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR | FTS_NOSTAT, nullptr), fts_close);
		if (!tree) {
			throw std::runtime_error("cannot open directory walk for " + root);
		}
		std::vector<WalkScope> scopes;
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
				auto scope = level ? scopes.back() : initial;
				const bool boundary = level && mounts.count(path);
				if (boundary || within(path, state)) {
					fts_set(tree.get(), entry, FTS_SKIP);
					continue;
				}

				const Fd dir = open_path(path, O_RDONLY | O_DIRECTORY);
				struct stat st {};
				if (!readable_scope_directory(dir, st)) {
					fts_set(tree.get(), entry, FTS_SKIP);
					continue;
				}
				const auto previous = scope.repo;
				select_scope(path, dir, st, policy, scope);
				if (scope.repo && (scope.repo != previous || !level)) {
					found(*scope.repo);
				}
				scopes.push_back(std::move(scope));
			} else if ((entry->fts_info == FTS_F || entry->fts_info == FTS_NSOK) &&
				level && scopes.size() >= level && scopes[level - 1].repo) {
				struct stat st {};
				if (!mounts.count(entry->fts_path) && lstat(entry->fts_path, &st) == 0 && S_ISREG(st.st_mode)) {
					file(*scopes[level - 1].repo, entry->fts_path, st);
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

Fd open_indexed(const string &path, const string &root, const string &repo, const Stamp &stamp,
	const ScanPolicy &policy)
{
	Fd dir = open_path(root, O_RDONLY | O_DIRECTORY);
	struct stat st {};
	const auto name = std::string_view(root).substr(root.find_last_of('/') + 1);
	if (fstat(dir, &st) < 0 || identity(st) != repo ||
		(!policy.storage_scope(root) && !git_repository(dir) && !artifact_directory(name, dir))) {
		return {};
	}

	// Open the member relative to the verified scope directory.
	Fd fd = open_path(path.substr(root == "/" ? 1 : root.size() + 1), O_RDONLY, dir);
	return current(fd, stamp) && writable_btrfs(fd) ? fd : Fd {};
}

using Extents = std::vector<crucible::FiemapExtent>;

Extents extent_map(int fd, uint64_t offset, uint64_t length)
{
	crucible::Fiemap map(offset, length);
	// Delayed or unknown allocations are excluded from the accounting.
	map.m_flags = 0;
	try {
		map.do_ioctl(fd);
	} catch (const std::system_error &) {
		return {};
	}
	return std::move(map.m_extents);
}

bool allocated(const crucible::FiemapExtent &extent)
{
	constexpr auto uncertain = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
		FIEMAP_EXTENT_UNWRITTEN | FIEMAP_EXTENT_DATA_INLINE;
	return extent.fe_physical && !(extent.fe_flags & uncertain);
}

uint64_t newly_shared(const Extents &before, int fd, uint64_t offset, uint64_t length)
{
	if (!length || std::none_of(before.begin(), before.end(), [](const auto &extent) {
		return allocated(extent) && !(extent.fe_flags & FIEMAP_EXTENT_SHARED);
	})) {
		return 0;
	}

	const auto after = extent_map(fd, offset, length);
	uint64_t bytes = 0;
	size_t i = 0, j = 0;
	while (i < before.size() && j < after.size()) {
		const auto &old = before[i];
		const auto &now = after[j];
		const auto old_end = old.fe_logical + old.fe_length;
		const auto new_end = now.fe_logical + now.fe_length;
		if (allocated(old) && !(old.fe_flags & FIEMAP_EXTENT_SHARED) &&
			allocated(now) && (now.fe_flags & FIEMAP_EXTENT_SHARED)) {
			const auto start = std::max({offset, static_cast<uint64_t>(old.fe_logical),
				static_cast<uint64_t>(now.fe_logical)});
			const auto end = std::min({offset + length, static_cast<uint64_t>(old_end),
				static_cast<uint64_t>(new_end)});
			if (end > start) {
				bytes += end - start;
			}
		}
		if (old_end <= new_end) {
			++i;
		}
		if (new_end <= old_end) {
			++j;
		}
	}
	return bytes;
}

string json_utf8(std::string_view data)
{
	string result;
	result.reserve(data.size());
	for (size_t i = 0; i < data.size();) {
		const auto lead = static_cast<unsigned char>(data[i]);
		size_t length = lead < 0x80 ? 1 :
			lead >= 0xc2 && lead <= 0xdf ? 2 :
			lead >= 0xe0 && lead <= 0xef ? 3 :
			lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
		bool valid = length && length <= data.size() - i;
		for (size_t j = 1; valid && j < length; ++j) {
			const auto next = static_cast<unsigned char>(data[i + j]);
			valid = next >= 0x80 && next <= 0xbf;
			if (j == 1) {
				valid = valid && !(lead == 0xe0 && next < 0xa0) &&
					!(lead == 0xed && next >= 0xa0) &&
					!(lead == 0xf0 && next < 0x90) && !(lead == 0xf4 && next >= 0x90);
			}
		}
		if (valid) {
			result.append(data.data() + i, length);
			i += length;
		} else {
			// Linux paths are byte strings. Keep database keys exact and replace
			// invalid bytes only in the JSON text displayed by consumers.
			result += "\\ufffd";
			++i;
		}
	}
	return result;
}

Integer unix_time()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

struct DedupEvent {
	Integer at;
	string source, destination;
	uint64_t bytes;
};

struct Activity {
	string phase = "stopped";
	string scan_kind = "full", scan_root;
	Integer phase_started_at = unix_time();
	Integer repositories = 0, scanned_files = 0, hashed_files = 0, indexed_chunks = 0;
	Integer processed_files = 0, matched_ranges = 0, rejected_ranges = 0, next_scan_at = 0;
};

// Publish a complete snapshot so readers can open it during any index transaction.
void publish_stats(sqlite3 *sql, const string &state, const Activity &activity,
	const std::vector<DedupEvent> &events)
{
	// SQLite handles JSON escaping, including paths with quotes or newlines.
	Statement event_json(sql, "SELECT json_object('at',?,'source',CAST(? AS TEXT),"
		"'destination',CAST(? AS TEXT),'bytes',?)");
	string recent = "[";
	for (const auto &event : events) {
		if (recent.size() > 1) {
			recent += ',';
		}
		event_json.bind(1, event.at).bind(2, event.source).bind(3, event.destination)
			.bind(4, event.bytes).next();
		recent += event_json.bytes(0);
		event_json.reset();
	}
	recent += ']';
	Statement snapshot(sql, R"SQL(
		WITH credit(path, dest_bytes, source_bytes) AS (
			SELECT path, 0, 0 FROM repositories
			UNION ALL SELECT destination, bytes, 0 FROM dedup_totals
			UNION ALL SELECT source, 0, bytes FROM dedup_totals
		), projects AS (
			SELECT path, sum(dest_bytes) AS bytes, sum(source_bytes) AS source_bytes
			FROM credit GROUP BY path ORDER BY bytes DESC, path
		), pairs AS (
			SELECT min(source, destination) AS a, max(source, destination) AS b,
				sum(bytes) AS bytes, sum(CASE WHEN destination < source THEN bytes ELSE 0 END) AS to_a
			FROM dedup_totals GROUP BY a,b ORDER BY bytes DESC, a,b
		)
		SELECT json_object(
			'version', 1,
			'startedAt', (SELECT started_at FROM stats_epoch),
			'updatedAt', unixepoch(),
			'totalBytes', (SELECT coalesce(sum(bytes),0) FROM dedup_totals),
			'rangeCount', (SELECT coalesce(sum(ranges),0) FROM dedup_totals),
			'activity', json_object(
				'phase', CAST(? AS TEXT), 'phaseStartedAt', ?,
				'repositories', ?, 'scannedFiles', ?, 'hashedFiles', ?, 'indexedChunks', ?,
				'processedFiles', ?, 'matchedRanges', ?, 'rejectedRanges', ?, 'nextScanAt', ?,
				'scanKind', CAST(? AS TEXT), 'scanRoot', CAST(? AS TEXT)),
			'recentEvents', json(CAST(? AS TEXT)),
			'projects', json((SELECT json_group_array(json_object(
				'path', CAST(path AS TEXT), 'bytes', bytes, 'sourceBytes', source_bytes)) FROM projects)),
			'pairs', json((SELECT json_group_array(json_object(
				'a', CAST(a AS TEXT), 'b', CAST(b AS TEXT), 'bytes', bytes, 'toA', to_a)) FROM pairs))
		)
	)SQL");
	snapshot.bind(1, activity.phase).bind(2, activity.phase_started_at)
		.bind(3, activity.repositories).bind(4, activity.scanned_files)
		.bind(5, activity.hashed_files).bind(6, activity.indexed_chunks)
		.bind(7, activity.processed_files).bind(8, activity.matched_ranges)
		.bind(9, activity.rejected_ranges).bind(10, activity.next_scan_at)
		.bind(11, activity.scan_kind).bind(12, activity.scan_root).bind(13, recent).next();
	auto data = json_utf8(snapshot.bytes(0));
	data.push_back('\n');
	string temp = state + "/.stats.XXXXXX";
	const Fd fd = mkstemp(temp.data());
	if (!fd) {
		throw std::system_error(errno, std::generic_category(), "create statistics snapshot");
	}
	try {
		crucible::write_or_die(fd, data);
		crucible::rename_or_die(temp, state + "/stats.json");
	} catch (...) {
		unlink(temp.c_str());
		throw;
	}
}

class Index {
	Fd lock;
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db {nullptr, sqlite3_close};
	const string state;
	const ScanPolicy &policy;
	Activity activity;
	std::vector<DedupEvent> recent_events;

	void exec(const char *sql) { check(sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr), db.get()); }

	void publish() { publish_stats(db.get(), state, activity, recent_events); }

	void phase(const string &name, long delay = 0)
	{
		activity.phase = name;
		activity.phase_started_at = unix_time();
		activity.next_scan_at = delay ? activity.phase_started_at + delay : 0;
		publish();
	}

public:
	Index(const string &directory, const ScanPolicy &scan_policy) : state(directory), policy(scan_policy)
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
		exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;"
			"PRAGMA cache_size = -8192; PRAGMA temp_store = FILE; PRAGMA foreign_keys = ON;"
			"CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, path TEXT UNIQUE, root TEXT,"
			" repo TEXT, fsid TEXT, seen INTEGER, pending INTEGER, device INTEGER, inode INTEGER,"
			" size INTEGER, msec INTEGER, mnsec INTEGER, csec INTEGER, cnsec INTEGER);"
			"CREATE INDEX IF NOT EXISTS files_pending ON files (pending, id);"
			"CREATE TEMP TABLE scan_seen (id INTEGER PRIMARY KEY) WITHOUT ROWID;"
			"CREATE TABLE IF NOT EXISTS chunks (file INTEGER REFERENCES files(id) ON DELETE CASCADE,"
			" start INTEGER, length INTEGER, hash INTEGER, PRIMARY KEY(file, start)) WITHOUT ROWID;"
			"CREATE INDEX IF NOT EXISTS chunk_match ON chunks (hash, length, file);"
			"CREATE TABLE IF NOT EXISTS repositories (path BLOB PRIMARY KEY, seen INTEGER) WITHOUT ROWID;"
			"CREATE TABLE IF NOT EXISTS dedup_totals (source BLOB, destination BLOB,"
			" bytes INTEGER NOT NULL, ranges INTEGER NOT NULL, PRIMARY KEY(source,destination)) WITHOUT ROWID;"
			"CREATE TABLE IF NOT EXISTS stats_epoch (id INTEGER PRIMARY KEY, started_at INTEGER);"
			"INSERT OR IGNORE INTO stats_epoch VALUES (1, unixepoch());");
		publish();
	}

	void scan(const std::vector<string> &roots, bool complete = true,
		const std::vector<string> *complete_roots = nullptr)
	{
		activity = Activity {};
		activity.scan_kind = complete ? "full" : "priority";
		activity.scan_root = !complete && roots.size() == 1 ? roots.front() : "";
		phase("scanning");
		auto *sql = db.get();
		// Keep scan membership temporary so unchanged files do not rewrite the index.
		exec("DELETE FROM scan_seen");
		// Repository epochs also work with an existing index; files.seen stays compatible.
		Statement latest(sql, "SELECT coalesce(max(seen),0) + 1 FROM repositories");
		latest.next();
		const auto round = latest.number(0);
		latest.reset();

		Statement existing(sql, "SELECT id,root,repo,fsid,device,inode,size,msec,mnsec,csec,cnsec"
			" FROM files WHERE path=?");
		Statement touch(sql, "INSERT OR IGNORE INTO scan_seen VALUES (?)");
		Statement remove(sql, "DELETE FROM files WHERE path=?");
		Statement add(sql, "INSERT INTO files(path,root,repo,fsid,seen,pending,device,inode,size,msec,mnsec,csec,cnsec)"
			" VALUES(?,?,?,?,?,1,?,?,?,?,?,?,?)");
		Statement add_chunk(sql, "INSERT INTO chunks VALUES (?,?,?,?)");
		Statement add_repo(sql, "INSERT INTO repositories VALUES (?,?)"
			" ON CONFLICT(path) DO UPDATE SET seen=excluded.seen");

		std::array<char, chunk_size> buffer;
		const std::array<char, chunk_size> zeros {};
		size_t batch = 0;
		auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		auto report_scan = [&]() {
			const auto now = std::chrono::steady_clock::now();
			if (now >= next_report) {
				publish();
				std::cout << "scanning repositories=" << activity.repositories
					<< " scanned_files=" << activity.scanned_files
					<< " hashed_files=" << activity.hashed_files
					<< " indexed_chunks=" << activity.indexed_chunks << std::endl;
				next_report = now + std::chrono::seconds(10);
			}
		};
		exec("BEGIN");
		walk(roots, state, policy, [&](const Repository &repo) {
			add_repo.bind(1, repo.path).bind(2, round).run();
			++activity.repositories;
		},
			[&](const Repository &repo, const string &path, const struct stat &st) {
			if (st.st_nlink != 1 || !st.st_blocks || st.st_size < static_cast<off_t>(repo.sector_size)) {
				return;
			}
			++activity.scanned_files;
			if (++batch == 512) {
				exec("COMMIT");
				batch = 0;
				report_scan();
				exec("BEGIN");
			}
			const Stamp stamp(st);
			existing.bind(1, path);
			const bool unchanged = existing.next() && existing.bytes(1) == repo.path &&
				existing.bytes(2) == repo.key && existing.bytes(3) == repo.fsid &&
				stamp.values == Stamp(existing, 4).values;
			if (unchanged) {
				touch.bind(1, existing.number(0)).run();
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
					++activity.indexed_chunks;
				}
				offset += len;
				report_scan();
			}
			if (!complete || !current(fd, stamp)) {
				remove.bind(1, path).run();
			} else {
				touch.bind(1, id).run();
				++activity.hashed_files;
			}
		}, complete_roots);
		// An interrupted walk has an incomplete membership set and must not prune.
		if (complete && !stopping) {
			Statement prune(sql, "DELETE FROM files WHERE id IN ("
				" SELECT id FROM files INDEXED BY files_pending"
				" WHERE id NOT IN (SELECT id FROM scan_seen))");
			prune.run();
			Statement prune_repositories(sql, "DELETE FROM repositories WHERE seen<>?");
			prune_repositories.bind(1, round).run();
		}
		exec("COMMIT");
		publish();
		std::cout << "scan repositories=" << activity.repositories << " hashed_files=" << activity.hashed_files
			<< " indexed_chunks=" << activity.indexed_chunks << (stopping ? " interrupted" : "") << std::endl;
	}

	void dedup(bool partial = false)
	{
		phase("deduplicating");
		auto *sql = db.get();
		Statement has_existing(sql, "SELECT 1 FROM files WHERE pending=0 LIMIT 1");
		const bool have_existing = has_existing.next();
		has_existing.reset();
		if (have_existing) {
			// Snapshot each new content key once. The repository bounds tell us
			// whether any new copy belongs to a different repository.
			// CROSS JOIN keeps this incremental lookup driven by the new keys.
			const string fresh_input = partial ?
				"scan_seen picked CROSS JOIN files f INDEXED BY files_pending ON f.id=picked.id"
				" CROSS JOIN chunks added ON added.file=f.id" :
				"files f JOIN chunks added ON added.file=f.id";
			const string wake = R"SQL(
				WITH fresh AS MATERIALIZED (
					SELECT added.hash, added.length, f.fsid,
						min(f.repo) AS first_repo, max(f.repo) AS last_repo
					FROM )SQL" + fresh_input + R"SQL(
					WHERE f.pending=1 GROUP BY added.hash, added.length, f.fsid
				)
				UPDATE files SET pending=1 WHERE id IN (
					SELECT old.id FROM fresh
					CROSS JOIN chunks match ON match.hash=fresh.hash AND match.length=fresh.length
					CROSS JOIN files old ON old.id=match.file
					WHERE old.pending=0 AND old.fsid=fresh.fsid
						AND (old.repo<>fresh.first_repo OR old.repo<>fresh.last_repo)
				)
			)SQL";
			exec(wake.c_str());
		}
		Statement pending(sql, partial ?
			"SELECT f.id,f.path,f.root,f.repo,f.fsid,f.device,f.inode,f.size,f.msec,f.mnsec,f.csec,f.cnsec"
			" FROM scan_seen picked CROSS JOIN files f INDEXED BY files_pending ON f.id=picked.id"
			" WHERE picked.id>? AND f.pending=1 ORDER BY picked.id LIMIT 1" :
			"SELECT id,path,root,repo,fsid,device,inode,size,msec,mnsec,csec,cnsec"
			" FROM files WHERE pending=1 AND id>? ORDER BY id LIMIT 1");
		Statement chunks(sql, "SELECT start,length,hash FROM chunks WHERE file=?");
		Statement sources(sql, "SELECT f.id,f.path,f.root,f.repo,f.device,f.inode,f.size,"
			" f.msec,f.mnsec,f.csec,f.cnsec,c.start FROM chunks c JOIN files f ON f.id=c.file"
			" WHERE c.hash=? AND c.length=? AND f.fsid=? AND f.repo<>? ORDER BY c.file,c.start");
		Statement consecutive(sql, R"SQL(
			SELECT d.start, d.length, s.hash=d.hash AND s.length=d.length AND NOT EXISTS (
				SELECT 1 FROM chunks earlier JOIN files f ON f.id=earlier.file
				WHERE earlier.hash=d.hash AND earlier.length=d.length AND earlier.file<=s.file
					AND (earlier.file<s.file OR earlier.start<s.start) AND f.fsid=?7 AND f.repo<>?8
			)
			FROM chunks d LEFT JOIN chunks s ON s.file=?1 AND s.start=?2+(d.start-?3)
			WHERE d.file=?4 AND d.start>=?5 AND d.start<?6 ORDER BY d.start
		)SQL");
		Statement done(sql, "UPDATE files SET pending=0 WHERE id=?");
		Statement record(sql, "INSERT INTO dedup_totals VALUES (?,?,?,1)"
			" ON CONFLICT(source,destination) DO UPDATE SET bytes=bytes+excluded.bytes, ranges=ranges+1");

		Integer id = 0;
		uint64_t bytes = 0, shared = 0;
		int last_error = 0;
		unsigned batch = 0;
		auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		exec("BEGIN");
		while (!stopping && pending.bind(1, id).next()) {
			id = pending.number(0);
			const auto root = pending.bytes(2);
			const auto repo = pending.bytes(3);
			const auto fsid = pending.bytes(4);
			const Stamp dest_stamp(pending, 5);
			const auto destination_path = pending.bytes(1);
			const Fd dest = open_indexed(destination_path, root, repo, dest_stamp, policy);
			pending.reset();
			bool retry = false;
			if (static_cast<int>(dest) >= 0) {
				chunks.bind(1, id);
				Fd source;
				Integer source_id = 0;
				Integer completed_until = 0;
				while (!stopping && chunks.next()) {
					const auto offset = chunks.number(0);
					const auto length = chunks.number(1);
					if (offset + length <= completed_until) {
						continue;
					}
					sources.bind(1, chunks.number(2)).bind(2, length).bind(3, fsid).bind(4, repo);
					while (!stopping && sources.next()) {
						if (source_id != sources.number(0)) {
							source_id = sources.number(0);
							source = open_indexed(sources.bytes(1), sources.bytes(2),
								sources.bytes(3), Stamp(sources, 4), policy);
						}
						if (!source) {
							continue;
						}
						const auto source_offset = sources.number(11);
						const auto limit = std::min(max_dedup_size, dest_stamp.values[2] - offset);
						auto requested = length;
						if (length == static_cast<Integer>(chunk_size) && length < limit) {
							// Keep the first source for every chunk, including repeated
							// content within a file, so batching preserves space savings.
							consecutive.bind(1, source_id).bind(2, source_offset).bind(3, offset)
								.bind(4, id).bind(5, offset + length).bind(6, offset + limit)
								.bind(7, fsid).bind(8, repo);
							while (consecutive.next()) {
								if (!consecutive.number(2) || consecutive.number(0) != offset + requested ||
									consecutive.number(1) > limit - requested) {
									break;
								}
								requested += consecutive.number(1);
							}
							consecutive.reset();
						}
						const auto before = extent_map(dest, offset, requested);
						int status = 0;
						uint64_t accepted = 0;
						for (;;) {
							crucible::BtrfsExtentSame same(source, source_offset, requested);
							same.add(dest, offset);
							try {
								same.do_ioctl();
								status = same.m_info.front().status;
								accepted = same.m_info.front().bytes_deduped;
							} catch (const std::system_error &error) {
								status = -error.code().value();
							}
							if (status == 0) {
								break;
							}
							++activity.rejected_ranges;
							last_error = status;
							if (status == -ENOSPC || status == -ENOMEM || status == -EAGAIN || status == -EINTR) {
								retry = true;
								break;
							}
							if (requested == length || stopping) {
								break;
							}
							// A stale later chunk must not hide a valid first chunk.
							requested = length;
						}
						if (retry) {
							break;
						}
						if (status == 0) {
							const auto added = newly_shared(before, dest, offset, accepted);
							if (added && current(dest, dest_stamp)) {
								record.bind(1, sources.bytes(2)).bind(2, root).bind(3, added).run();
								shared += added;
								const auto source_path = sources.bytes(1);
								if (!recent_events.empty() && recent_events.back().source == source_path &&
									recent_events.back().destination == destination_path) {
									recent_events.back().at = unix_time();
									recent_events.back().bytes += added;
								} else {
									if (recent_events.size() == 32) {
										recent_events.erase(recent_events.begin());
									}
									recent_events.push_back({unix_time(), source_path, destination_path, added});
								}
							}
							bytes += accepted;
							++activity.matched_ranges;
							completed_until = offset + accepted;
							retry = accepted < static_cast<uint64_t>(length);
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
			if (!stopping) {
				++activity.processed_files;
			}
			const auto now = std::chrono::steady_clock::now();
			if (++batch == 512 || now >= next_report) {
				exec("COMMIT");
				if (now >= next_report) {
					publish();
					std::cout << "deduplicating processed_files=" << activity.processed_files
						<< " matched_ranges=" << activity.matched_ranges
						<< " newly_shared_bytes=" << shared << std::endl;
					next_report = now + std::chrono::seconds(10);
				}
				exec("BEGIN");
				batch = 0;
			}
		}
		pending.reset();
		exec("COMMIT");
		publish();
		std::cout << "dedup ranges=" << activity.matched_ranges << " kernel_bytes=" << bytes
			<< " newly_shared_bytes=" << shared
			<< " rejected=" << activity.rejected_ranges << " last_status=" << last_error
			<< (stopping ? " interrupted" : "") << std::endl;
	}

	void idle(long interval) { phase("idle", interval); }

	void finish() { phase("stopped"); }
};

string state_directory()
{
	const auto *state = std::getenv("XDG_STATE_HOME");
	if (state && *state == '/') {
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
	std::vector<string> roots, priority_roots;
	string state;
	bool once = false, discover = false, stats = false;
	long interval = 300;
	const option options[] = {
		{"root", required_argument, nullptr, 'r'},
		{"priority-root", required_argument, nullptr, 'p'},
		{"state-dir", required_argument, nullptr, 's'},
		{"interval", required_argument, nullptr, 'i'},
		{"once", no_argument, nullptr, '1'},
		{"discover", no_argument, nullptr, 'd'},
		{"stats", no_argument, nullptr, 't'},
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0},
	};
	for (int opt; (opt = getopt_long(argc, argv, "r:p:s:i:1dth", options, nullptr)) != -1;) {
		switch (opt) {
		case 'r':
			roots.push_back(fs::canonical(optarg).string());
			if (!fs::is_directory(roots.back())) {
				throw std::runtime_error("--root expects a directory");
			}
			break;
		case 'p':
			priority_roots.push_back(fs::canonical(optarg).string());
			if (!fs::is_directory(priority_roots.back())) {
				throw std::runtime_error("--priority-root expects a directory");
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
		case 't': stats = true; break;
		case 'h':
			std::cout << "Usage: project-bees [--root DIR]... [--state-dir DIR]\n"
				"                    [--priority-root DIR]...\n"
				"                    [--interval SECONDS] [--once | --discover | --stats]\n"
				"Discover Git repositories and standalone build/dependency directories on mounted\n"
				"writable Btrfs filesystems and deduplicate ranges across scopes.\n"
				"Default system scans process build caches and user data directories first.\n"
				"--priority-root adds an early directory pass within any --root limit.\n"
				"The default interval is 300 seconds after each pass.\n"
				"--stats prints the daemon's cumulative statistics as JSON.\n";
			return 0;
		default: return 2;
		}
	}
	if (optind != argc || once + discover + stats > 1) {
		throw std::runtime_error("invalid arguments; see --help");
	}
	std::signal(SIGINT, stop);
	std::signal(SIGTERM, stop);
	umask(0077);
	state = fs::weakly_canonical(fs::absolute(state.empty() ? state_directory() : state)).string();
	if (stats) {
		std::ifstream report(state + "/stats.json");
		if (!report) {
			throw std::runtime_error("statistics unavailable; run project-bees to initialize this state directory");
		}
		std::cout << report.rdbuf();
		return std::cout ? 0 : 1;
	}
	const ScanPolicy policy(priority_roots);
	if (discover) {
		walk(roots, state, policy, [](const Repository &repo) { std::cout << repo.path << '\n'; },
			[](const Repository &, const string &, const struct stat &) {});
		return 0;
	}
	fs::create_directories(state);
	Index index(state, policy);
	std::cout << "project-bees running, scan interval=" << interval << "s" << std::endl;
	while (!stopping) {
		for (const auto &path : policy.priority_roots(roots)) {
			// Explicitly limited scans keep their original single-pass behavior
			// unless the caller requests an early pass as well.
			if (!roots.empty() && std::find(priority_roots.begin(), priority_roots.end(), path) == priority_roots.end()) {
				continue;
			}
			if (stopping) {
				break;
			}
			std::cout << "priority scan root=" << path << std::endl;
			index.scan({path}, false, &roots);
			if (!stopping) {
				index.dedup(true);
			}
		}
		if (stopping) {
			break;
		}
		std::cout << "full scan" << std::endl;
		index.scan(roots);
		if (!stopping) {
			index.dedup();
		}
		if (once || stopping) {
			break;
		}
		index.idle(interval);
		for (long seconds = 0; seconds < interval && !stopping; ++seconds) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
	index.finish();
	return 0;
} catch (const std::exception &error) {
	std::cerr << "project-bees: " << error.what() << '\n';
	return 1;
}
