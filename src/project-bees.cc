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
#include <optional>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/vfs.h>
#include <thread>
#include <tuple>
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

bool ascii_equal(std::string_view a, std::string_view b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
		return lower(x) == lower(y);
	});
}

bool backup_delimiter(char c)
{
	return c == '.' || c == '-' || c == '_' || c == ' ';
}

bool backup_container(std::string_view name)
{
	if (!name.empty() && name.front() == '.') {
		name.remove_prefix(1);
	}
	return ascii_equal(name, "backup") || ascii_equal(name, "backups") ||
		ascii_equal(name, "bak") || name == "备份";
}

std::string_view without_backup_date(std::string_view name)
{
	// Dates qualify a backup marker; an ordinary dated name is not itself a backup.
	if (name.empty() || name.back() < '0' || name.back() > '9') {
		return name;
	}
	for (size_t start = 1; start < name.size(); ++start) {
		if (!backup_delimiter(name[start - 1])) {
			continue;
		}
		string digits;
		for (const char c : name.substr(start)) {
			if (c >= '0' && c <= '9') {
				digits += c;
			} else if (!backup_delimiter(c) && c != ':' && c != 'T' && c != 't') {
				digits.clear();
				break;
			}
		}
		if (digits.size() != 8 && digits.size() != 14) {
			continue;
		}
		const auto number = [&](size_t offset, size_t length) {
			unsigned result = 0;
			for (const char c : std::string_view(digits).substr(offset, length)) {
				result = result * 10 + c - '0';
			}
			return result;
		};
		if (number(0, 4) >= 1900 && number(0, 4) <= 2999 &&
			number(4, 2) >= 1 && number(4, 2) <= 12 && number(6, 2) >= 1 && number(6, 2) <= 31 &&
			(digits.size() == 8 || (number(8, 2) < 24 && number(10, 2) < 60 && number(12, 2) < 60))) {
			return name.substr(0, start - 1);
		}
	}
	return name;
}

bool backup_name(std::string_view name, string *original = nullptr)
{
	name = without_backup_date(name);
	std::string_view base;
	bool matched = backup_container(name);
	if (!matched && name.size() > 1 && name.back() == '~') {
		base = name.substr(0, name.size() - 1);
		matched = true;
	}
	for (const auto suffix : {".bak", ".backup", ".old", ".orig", ".save"}) {
		const auto length = std::char_traits<char>::length(suffix);
		if (!matched && name.size() > length && ascii_equal(name.substr(name.size() - length), suffix)) {
			base = name.substr(0, name.size() - length);
			matched = true;
		}
	}
	for (const auto marker : {"backup", "backups", "bak", "备份"}) {
		const auto length = std::char_traits<char>::length(marker);
		if (matched || name.size() <= length) {
			continue;
		}
		if (ascii_equal(name.substr(0, length), marker) && backup_delimiter(name[length])) {
			base = name.substr(length + 1);
			matched = true;
		} else if (ascii_equal(name.substr(name.size() - length), marker) &&
			backup_delimiter(name[name.size() - length - 1])) {
			base = name.substr(0, name.size() - length - 1);
			matched = true;
		}
	}
	if (original) {
		*original = base == "." || base == ".." ? string() : string(base);
	}
	return matched;
}

string backup_original_name(int dir, std::string_view backup, mode_t type)
{
	string original;
	if (!backup_name(backup, &original) || original.empty()) {
		return {};
	}
	// A dated original is still an exact sibling; strip a date only as a fallback.
	if (is_entry(dir, original.c_str(), type)) {
		return original;
	}
	const string undated(without_backup_date(original));
	return undated != original && !undated.empty() && undated != "." && undated != ".." &&
		is_entry(dir, undated.c_str(), type) ? undated : string();
}

string readme_marker(int dir)
{
	for (const auto *name : {"README.md", "readme.md", "Readme.md", "README.MD"}) {
		if (is_entry(dir, name, S_IFREG)) {
			return name;
		}
	}
	const int copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (copy < 0) {
		return {};
	}
	const auto close_directory = [](DIR *value) { closedir(value); };
	std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(copy), close_directory);
	if (!entries) {
		close(copy);
		return {};
	}
	while (const auto *entry = readdir(entries.get())) {
		if (ascii_equal(entry->d_name, "README.md") && is_entry(dir, entry->d_name, S_IFREG)) {
			return entry->d_name;
		}
	}
	return {};
}

string build_manifest(int dir)
{
	for (const auto *name : {"Makefile", "package.json", "pom.xml", "build.gradle",
		"build.gradle.kts", "pubspec.yaml", "CMakeLists.txt", "build.zig", "meson.build",
		"Cargo.toml", "pyproject.toml", "setup.py", "composer.json", "go.mod", "Gemfile",
		"Package.swift", "mix.exs", "rebar.config"}) {
		if (is_entry(dir, name, S_IFREG)) {
			return name;
		}
	}
	const int copy = fcntl(dir, F_DUPFD_CLOEXEC, 0);
	if (copy < 0) {
		return {};
	}
	const auto close_directory = [](DIR *value) { closedir(value); };
	std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(copy), close_directory);
	if (!entries) {
		close(copy);
		return {};
	}
	while (const auto *entry = readdir(entries.get())) {
		const std::string_view name(entry->d_name);
		const auto dot = name.find_last_of('.');
		if (dot != name.npos) {
			const auto suffix = name.substr(dot);
			if ((suffix == ".csproj" || suffix == ".fsproj" || suffix == ".vbproj" || suffix == ".vcxproj") &&
				is_entry(dir, entry->d_name, S_IFREG)) {
				return string(name);
			}
		}
	}
	return {};
}

bool artifact_name(std::string_view name)
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
	return false;
}

string artifact_marker(std::string_view name, int dir, const string *readme)
{
	for (const auto *marker : {"pyvenv.cfg", "CMakeCache.txt"}) {
		if (is_entry(dir, marker, S_IFREG)) {
			return marker;
		}
	}
	if (readme && !readme->empty()) {
		return *readme;
	}
	for (const auto *candidate : {"build", "dist", "bin", "obj", "out", "artifacts", "vendor", "deps"}) {
		if (name == candidate) {
			const auto marker = build_manifest(dir);
			if (!marker.empty()) {
				return marker;
			}
			Fd parent = open_path("..", O_RDONLY | O_DIRECTORY, dir);
			const auto parent_marker = static_cast<int>(parent) >= 0 ? build_manifest(parent) : string();
			if (!parent_marker.empty()) {
				return "../" + parent_marker;
			}
			break;
		}
	}
	return readme ? string() : readme_marker(dir);
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

std::string_view parent_directory(std::string_view path)
{
	const auto slash = path.find_last_of('/');
	return slash == path.npos ? std::string_view() : path.substr(0, slash ? slash : 1);
}

enum class ManagedScope { none, container, version };

class ScanPolicy {
	struct Marker {
		string identity;
		string name;
	};
	std::vector<string> priority;
	std::set<string, std::less<>> storage;
	std::set<string, std::less<>> containers;
	std::map<string, unsigned, std::less<>> managed;
	std::set<string, std::less<>> file_containers;
	std::set<string, std::less<>> limits;
	mutable std::map<string, Marker, std::less<>> markers;
	mutable std::set<string, std::less<>> observed_scopes;
	mutable std::set<string, std::less<>> rejected_scopes;

	void prioritize(const string &path)
	{
		if (std::find(priority.begin(), priority.end(), path) == priority.end()) {
			priority.push_back(path);
		}
	}

	void add(const string &path, bool container = true)
	{
		auto normalized = absolute_directory(path);
		if (!normalized.empty()) {
			if (container) {
				containers.insert(normalized);
			}
			if (storage.insert(normalized).second) {
				prioritize(normalized);
			}
		}
	}

	void add_managed(const string &path, unsigned depth)
	{
		const auto normalized = absolute_directory(path);
		if (!normalized.empty()) {
			managed[normalized] = depth;
			prioritize(normalized);
		}
	}

	void add_files(const string &path)
	{
		const auto normalized = absolute_directory(path);
		if (!normalized.empty()) {
			file_containers.insert(normalized);
			prioritize(normalized);
		}
	}

	static std::string_view managed_boundary(std::string_view directory, std::string_view base, unsigned depth)
	{
		auto end = base.size();
		for (unsigned level = 0; level < depth && end < directory.size(); ++level) {
			const auto start = end + (end == 1 ? 0 : 1);
			// Manager metadata and temporary directories are not installed versions.
			if (directory[start] == '.') {
				break;
			}
			const auto slash = directory.find('/', start);
			end = slash == directory.npos ? directory.size() : slash;
		}
		return directory.substr(0, end);
	}

public:
	explicit ScanPolicy(const std::vector<string> &extra_priority_roots = {}, const std::vector<string> &roots = {})
		: limits(roots.begin(), roots.end())
	{
		for (const auto &path : extra_priority_roots) {
			add(fs::absolute(path).string(), false);
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
		const auto mise = environment_directory("MISE_DATA_DIR", child_directory(data, "mise"));
		const auto asdf = environment_directory("ASDF_DATA_DIR", child_directory(home, ".asdf"));
		const auto nvm_config = environment_directory("XDG_CONFIG_HOME", {});
		const auto nvm = environment_directory("NVM_DIR", nvm_config.empty() ?
			child_directory(home, ".nvm") : child_directory(nvm_config, "nvm"));
		add_managed(environment_directory("MISE_INSTALLS_DIR", child_directory(mise, "installs")), 2);
		add_managed(environment_directory("MISE_DOWNLOADS_DIR", child_directory(mise, "downloads")), 2);
		add_managed(environment_directory("UV_PYTHON_INSTALL_DIR", child_directory(data, "uv/python")), 1);
		add_managed(child_directory(nvm, "versions/node"), 1);
		add_managed(child_directory(environment_directory("PYENV_ROOT", child_directory(home, ".pyenv")), "versions"), 1);
		add_managed(child_directory(environment_directory("RBENV_ROOT", child_directory(home, ".rbenv")), "versions"), 1);
		add_managed(child_directory(environment_directory("SDKMAN_DIR", child_directory(home, ".sdkman")), "candidates"), 2);
		add_managed(child_directory(asdf, "installs"), 2);
		add_managed(child_directory(asdf, "downloads"), 2);
		add_files(child_directory(home, ".local/bin"));
		add_files(child_directory(cargo, "bin"));
		add_files(child_directory(data, "claude/versions"));
		add(child_directory(home, ".var/app"));
		add("/var/tmp");
		add("/var/backups");
		add(child_directory(home, "Backups"));
		add(child_directory(home, "backups"));
		add(child_directory(home, "备份"));
		add("/opt");
		add(child_directory(home, "opt"));
		add(child_directory(home, ".local/opt"));
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
		return storage.count(path) || storage.count(parent_directory(path));
	}

	bool storage_container(const string &path) const { return containers.count(path); }

	ManagedScope managed_scope(const string &path) const
	{
		std::string_view ancestor(path);
		for (unsigned distance = 0; distance <= 2 && !ancestor.empty(); ++distance) {
			const auto found = managed.find(ancestor);
			if (found != managed.end()) {
				if (managed_boundary(path, ancestor, found->second).size() != path.size()) {
					return ManagedScope::none;
				}
				return distance < found->second ? ManagedScope::container :
					distance == found->second ? ManagedScope::version : ManagedScope::none;
			}
			if (ancestor == "/") {
				break;
			}
			ancestor = parent_directory(ancestor);
		}
		return ManagedScope::none;
	}

	bool file_scope(const string &path) const { return file_containers.count(parent_directory(path)); }

	bool allowed_path(const string &path) const
	{
		return limits.empty() || std::any_of(limits.begin(), limits.end(),
			[&](const auto &root) { return within(path, root); });
	}

	string scope_boundary(const string &path, const Mounts &mounts) const
	{
		if (file_scope(path)) {
			return path;
		}
		const auto parent = parent_directory(path);
		auto ancestor = parent;
		while (!ancestor.empty()) {
			const auto found = managed.find(ancestor);
			if (found != managed.end()) {
				const auto boundary = managed_boundary(parent, ancestor, found->second);
				// A local --root or a separate mount starts a new traversal and does
				// not inherit the version owner outside that traversal's boundary.
				for (auto entry = parent; entry.size() > boundary.size(); entry = parent_directory(entry)) {
					const auto mount = mounts.find(string(entry));
					if (limits.count(entry) || (mount != mounts.end() && mount->second)) {
						return string(entry);
					}
				}
				return string(boundary);
			}
			if (ancestor == "/") {
				break;
			}
			ancestor = parent_directory(ancestor);
		}
		return {};
	}

	void observe_scope(const string &path) const { observed_scopes.insert(path); }
	bool observed_scope(const string &path) const { return observed_scopes.count(path); }
	void reject_scope(const string &path) const { rejected_scopes.insert(path); }
	bool rejected_scope(const string &path) const { return rejected_scopes.count(path); }

	void begin_scan() const
	{
		markers.clear();
		observed_scopes.clear();
		rejected_scopes.clear();
	}

	bool artifact_scope(const string &path, int dir, const struct stat &st, const string *readme = nullptr) const
	{
		const auto name = std::string_view(path).substr(path.find_last_of('/') + 1);
		if (artifact_name(name)) {
			return true;
		}
		const auto key = identity(st);
		const string no_readme;
		const auto supplied = storage_container(path) ? &no_readme : readme;
		if (readme) {
			// The walk already has the exact README name, including unusual casing.
			// Only selected roots need evidence retained; ordinary directories do not.
			const auto marker = artifact_marker(name, dir, supplied);
			if (marker.empty()) {
				markers.erase(path);
			} else {
				markers[path] = {key, marker};
			}
			return !marker.empty();
		}
		auto [cached, inserted] = markers.try_emplace(path, Marker {key, {}});
		if (inserted || cached->second.identity != key) {
			cached->second = {key, artifact_marker(name, dir, supplied)};
		} else if (!cached->second.name.empty()) {
			if (is_entry(dir, cached->second.name.c_str(), S_IFREG)) {
				return true;
			}
			cached->second.name = artifact_marker(name, dir, supplied);
		}
		// Revalidate the precise marker on every open. A failed lookup remains
		// negative for this pass, avoiding a full directory search for every file.
		return !cached->second.name.empty() && is_entry(dir, cached->second.name.c_str(), S_IFREG);
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

struct DirectoryNames {
	std::map<string, string, std::less<>> originals;
	string readme;
};

string qualified_backup_original(int dir, const string &path, const char *name,
	const string &state, const Mounts &mounts)
{
	string original;
	if (!backup_name(name, &original) || original.empty()) {
		return {};
	}
	const auto backup_path = (fs::path(path) / name).string();
	if (within(backup_path, state) || mounts.count(backup_path)) {
		return {};
	}
	struct stat backup {}, source {};
	if (fstatat(dir, name, &backup, AT_SYMLINK_NOFOLLOW) < 0 ||
		(!S_ISREG(backup.st_mode) && !S_ISDIR(backup.st_mode))) {
		return {};
	}
	original = backup_original_name(dir, name, backup.st_mode & S_IFMT);
	if (original.empty()) {
		return {};
	}
	const auto original_path = (fs::path(path) / original).string();
	if (within(original_path, state) || mounts.count(original_path) ||
		fstatat(dir, original.c_str(), &source, AT_SYMLINK_NOFOLLOW) < 0 ||
		(backup.st_mode & S_IFMT) != (source.st_mode & S_IFMT)) {
		return {};
	}
	if (S_ISREG(backup.st_mode)) {
		if (backup.st_nlink != 1 || source.st_nlink != 1) {
			return {};
		}
	} else if (S_ISDIR(backup.st_mode)) {
		for (const auto &candidate : {string(name), original}) {
			const Fd fd = open_path(candidate, O_RDONLY | O_DIRECTORY, dir);
			struct stat st {};
			if (!readable_scope_directory(fd, st)) {
				return {};
			}
		}
	} else {
		return {};
	}
	return original;
}

void directory_name(DirectoryNames &names, int dir, const string &path, const char *name,
	const string &state, const Mounts &mounts)
{
	if (ascii_equal(name, "README.md") && is_entry(dir, name, S_IFREG)) {
		names.readme = name;
	}
	const auto original = qualified_backup_original(dir, path, name, state, mounts);
	if (original.empty()) {
		return;
	}
	auto [position, inserted] = names.originals.emplace(original, name);
	if (!inserted && position->second > name) {
		position->second = name;
	}
}

string original_backup(const string &path, const string &state, const Mounts &mounts)
{
	const auto parent = fs::path(path).parent_path().string();
	const Fd dir = open_path(parent, O_RDONLY | O_DIRECTORY);
	const int copy = fcntl(dir, F_DUPFD_CLOEXEC, 0);
	if (copy < 0) {
		return {};
	}
	const auto close_directory = [](DIR *value) { closedir(value); };
	std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(copy), close_directory);
	if (!entries) {
		close(copy);
		return {};
	}
	DirectoryNames names;
	while (!stopping) {
		errno = 0;
		const auto *entry = readdir(entries.get());
		if (!entry) {
			if (errno) {
				throw std::system_error(errno, std::generic_category(), "backup siblings " + parent);
			}
			break;
		}
		directory_name(names, dir, parent, entry->d_name, state, mounts);
	}
	const auto found = names.originals.find(fs::path(path).filename().string());
	return found == names.originals.end() ? string() : found->second;
}

bool backup_storage_scope(const string &path)
{
	return backup_container(fs::path(path).parent_path().filename().string());
}

std::shared_ptr<Repository> make_scope(const string &path, int fd, const struct stat &st,
	const string &backup = {})
{
	crucible::BtrfsIoctlFsInfoArgs info;
	if (!info.do_ioctl_nothrow(fd) || !info.sectorsize || info.sectorsize > chunk_size) {
		return {};
	}
	const auto uuid = info.fsid();
	// An exact sibling witness makes an otherwise ordinary original independently
	// eligible without admitting its parent, and can be rechecked without a search.
	const auto key = identity(st) + (backup.empty() ? string() : ":" + backup);
	return std::make_shared<Repository>(Repository {
		path, key, string(uuid.begin(), uuid.end()), info.sectorsize,
	});
}

void select_scope(const string &path, int dir, const struct stat &st, const ScanPolicy &policy, WalkScope &scope,
	const string &original = {}, const string *readme = nullptr)
{
	const auto name = std::string_view(path).substr(path.find_last_of('/') + 1);
	const bool backup = backup_name(name);
	if (scope.repo && name == ".git") {
		scope.metadata = true;
	}
	if (scope.metadata && !backup) {
		return;
	}
	const auto managed = policy.managed_scope(path);
	const bool git = !scope.metadata && git_repository(dir);
	// A README describing a known cache/application container must not collapse
	// its application scopes. Explicit project priorities retain normal markers.
	const bool artifact = managed == ManagedScope::none && !git && !backup && (!scope.repo || scope.storage) &&
		policy.artifact_scope(path, dir, st, readme);
	const bool storage = managed == ManagedScope::none && !git && !backup && !artifact && (!scope.repo || scope.storage) &&
		(policy.storage_scope(path) || backup_storage_scope(path));
	const bool paired = managed == ManagedScope::none && !git && !backup && !artifact && !storage &&
		(!scope.repo || scope.storage) && !original.empty();
	if (managed != ManagedScope::none || git || backup || artifact || storage || paired) {
		if (auto selected = make_scope(path, dir, st, paired ? original : string())) {
			scope.repo = std::move(selected);
			// Only exact manager installation boundaries override an enclosing project.
			// Their version directories own their nested build/dependency directories.
			scope.storage = managed == ManagedScope::container || storage || (!git && backup_container(name));
			if (backup) {
				scope.metadata = false;
			}
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
		select_scope(path, dir, st, policy, scope,
			path == boundary ? string() : original_backup(path, state, mounts));
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
		std::vector<DirectoryNames> names;
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
				names.resize(level);
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
				string original;
				if (level) {
					const auto candidate = names.back().originals.find(entry->fts_name);
					if (candidate != names.back().originals.end()) {
						original = candidate->second;
					}
				} else if (complete_roots && std::any_of(boundaries.begin(), boundaries.end(),
					[&](const auto &boundary) { return path != boundary && within(path, boundary); })) {
					original = original_backup(path, state, mounts);
				}
				DirectoryNames children;
				errno = 0;
				auto *child = fts_children(tree.get(), 0);
				if (!child && errno) {
					throw std::system_error(errno, std::generic_category(), "directory children " + path);
				}
				for (; child && !stopping; child = child->fts_link) {
					directory_name(children, dir, path, child->fts_name, state, mounts);
				}
				select_scope(path, dir, st, policy, scope, original, &children.readme);
				if (scope.repo && (scope.repo != previous || !level)) {
					policy.observe_scope(scope.repo->path);
					found(*scope.repo);
				}
				scopes.push_back(std::move(scope));
				names.push_back(std::move(children));
			} else if ((entry->fts_info == FTS_F || entry->fts_info == FTS_NSOK) &&
				level && scopes.size() >= level) {
				auto repo = scopes[level - 1].repo;
				const auto original = names[level - 1].originals.find(entry->fts_name);
				const bool backup = backup_name(entry->fts_name);
				const bool independent = policy.file_scope(entry->fts_path);
				if (!repo && !backup && !independent && original == names[level - 1].originals.end()) {
					continue;
				}
				struct stat st {};
				if (!mounts.count(entry->fts_path) && lstat(entry->fts_path, &st) == 0 && S_ISREG(st.st_mode)) {
					if (st.st_nlink == 1 && (backup || independent || (!repo && original != names[level - 1].originals.end()))) {
						const Fd fd = open_path(entry->fts_path, O_RDONLY);
						repo = make_scope(entry->fts_path, fd, st, backup || independent ? string() : original->second);
						if (repo) {
							policy.observe_scope(repo->path);
							found(*repo);
						}
					}
					if (repo) {
						file(*repo, entry->fts_path, st);
					}
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

struct SourceCandidate {
	Integer id;
	string path, root, repo;
	Stamp stamp;
	Integer offset;

	explicit SourceCandidate(const Statement &row) :
		id(row.number(0)), path(row.bytes(1)), root(row.bytes(2)), repo(row.bytes(3)),
		stamp(row, 4), offset(row.number(11)) {}
};

class FirstSourceCache {
	using Key = std::tuple<Integer, Integer, string, string, string>;
	using Entries = std::map<Key, std::optional<SourceCandidate>, std::less<>>;
	// Keep only a small number of rows, including empty lookups. FIFO eviction
	// bounds memory without retaining source descriptors or qualification results.
	static constexpr size_t capacity = 64;
	Entries entries;
	std::array<Entries::iterator, capacity> order {};
	size_t next = 0;

public:
	const std::optional<SourceCandidate> &get(Statement &sources, Integer hash, Integer length,
		const string &fsid, const string &repo, const string &root, bool &query_started)
	{
		query_started = false;
		const auto found = entries.find(std::tie(hash, length, fsid, repo, root));
		if (found != entries.end()) {
			return found->second;
		}
		std::optional<SourceCandidate> first;
		sources.bind(1, hash).bind(2, length).bind(3, fsid).bind(4, repo).bind(5, root);
		if (sources.next()) {
			first.emplace(sources);
			// Preserve the live cursor for fallback on a miss, so a rejected
			// first row does not require executing this same query a second time.
			query_started = true;
		} else {
			sources.reset();
		}
		if (entries.size() == capacity) {
			entries.erase(order[next]);
		}
		const auto inserted = entries.emplace(Key {hash, length, fsid, repo, root}, std::move(first)).first;
		order[next] = inserted;
		next = (next + 1) % capacity;
		return inserted->second;
	}
};

bool current(int fd, const Stamp &stamp)
{
	struct stat st {};
	return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
		Stamp(st).values == stamp.values;
}

bool current_managed_scope(const string &root, const string &boundary, const string &state,
	const ScanPolicy &policy, const Mounts &mounts, bool original_file = false)
{
	if (root == boundary || policy.observed_scope(root)) {
		return true;
	}
	if (policy.rejected_scope(root)) {
		return false;
	}
	// Old artifacts such as node_modules may now belong to their installation.
	// Replay only unvisited deeper roots; normal version roots need no extra IO.
	std::vector<string> ancestors;
	for (auto path = original_file ? fs::path(root).parent_path() : fs::path(root);; path = path.parent_path()) {
		ancestors.push_back(path.string());
		if (path.string() == boundary) {
			break;
		}
	}
	std::reverse(ancestors.begin(), ancestors.end());
	WalkScope scope;
	for (const auto &path : ancestors) {
		if (within(path, state) || (path != boundary && mounts.count(path))) {
			return false;
		}
		const Fd dir = open_path(path, O_RDONLY | O_DIRECTORY);
		struct stat st {};
		if (!readable_scope_directory(dir, st)) {
			return false;
		}
		const auto previous = scope.repo;
		select_scope(path, dir, st, policy, scope);
		if (path != boundary && scope.repo == previous && (!scope.repo || scope.storage)) {
			const auto original = original_backup(path, state, mounts);
			if (!original.empty()) {
				select_scope(path, dir, st, policy, scope, original);
			}
		}
	}
	const bool valid = original_file ? !scope.repo : scope.repo && scope.repo->path == root;
	if (!valid) {
		policy.reject_scope(root);
	}
	return valid;
}

Fd open_indexed(const string &path, const string &root, const string &repo, const Stamp &stamp,
	const ScanPolicy &policy, const string &state, const Mounts &mounts)
{
	if (!within(path, root) || within(path, state) || !policy.allowed_path(path)) {
		return {};
	}
	const auto boundary = policy.scope_boundary(path, mounts);
	// Partial scans can contain both old and new ownership records. Reject
	// an old ancestor without letting it source files in the same new scope.
	if (!boundary.empty() && root.size() < boundary.size()) {
		return {};
	}
	Fd dir = open_path(root, O_RDONLY);
	struct stat st {};
	const auto name = std::string_view(root).substr(root.find_last_of('/') + 1);
	const auto separator = repo.find(':');
	if (fstat(dir, &st) < 0 || identity(st) != repo.substr(0, separator)) {
		return {};
	}
	bool original = false;
	if (separator != string::npos) {
		const auto backup = repo.substr(separator + 1);
		const auto parent_path = fs::path(root).parent_path().string();
		const Fd parent = open_path(parent_path, O_RDONLY | O_DIRECTORY);
		original = qualified_backup_original(parent, parent_path, backup.c_str(), state, mounts) == name;
	}
	if (S_ISREG(st.st_mode)) {
		if (original && !backup_name(name) && !policy.file_scope(root) && !boundary.empty() &&
			!current_managed_scope(root, boundary, state, policy, mounts, true)) {
			return {};
		}
		return path == root && (backup_name(name) || original || policy.file_scope(root)) &&
			current(dir, stamp) && writable_btrfs(dir)
			? dir : Fd {};
	}
	if (!S_ISDIR(st.st_mode) || !readable_scope_directory(dir, st) || !within(path, root) || path == root ||
		(!original && !backup_name(name) && !backup_storage_scope(root) && !policy.storage_scope(root) &&
			policy.managed_scope(root) == ManagedScope::none && !git_repository(dir) && !policy.artifact_scope(root, dir, st))) {
		return {};
	}
	if (!boundary.empty() && !current_managed_scope(root, boundary, state, policy, mounts)) {
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
		policy.begin_scan();
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
		Statement change_owner(sql, "UPDATE files SET root=?,repo=?,pending=1 WHERE id=?");
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
			const bool indexed = existing.next();
			const auto previous_id = indexed ? existing.number(0) : 0;
			const bool same_content = indexed && existing.bytes(3) == repo.fsid &&
				stamp.values == Stamp(existing, 4).values;
			const bool unchanged = same_content && existing.bytes(1) == repo.path &&
				existing.bytes(2) == repo.key;
			if (unchanged) {
				touch.bind(1, previous_id).run();
			}
			existing.reset();
			if (unchanged) {
				return;
			}

			Fd fd = open_path(path, O_RDONLY);
			if (!current(fd, stamp)) {
				return;
			}
			if (same_content) {
				// Scope discovery may change an unchanged file's owner. Its
				// committed chunk index is still valid, but the new scope must
				// participate in pending wake-up and candidate selection.
				change_owner.bind(1, repo.path).bind(2, repo.key).bind(3, previous_id).run();
				touch.bind(1, previous_id).run();
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
		const auto mounts = read_mounts();
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
						min(f.repo) AS first_repo, max(f.repo) AS last_repo,
						min(f.root) AS first_root, max(f.root) AS last_root
					FROM )SQL" + fresh_input + R"SQL(
					WHERE f.pending=1 GROUP BY added.hash, added.length, f.fsid
				)
				UPDATE files SET pending=1 WHERE id IN (
					SELECT old.id FROM fresh
					CROSS JOIN chunks match ON match.hash=fresh.hash AND match.length=fresh.length
					CROSS JOIN files old ON old.id=match.file
					WHERE old.pending=0 AND old.fsid=fresh.fsid
						AND (old.repo<>fresh.first_repo OR old.repo<>fresh.last_repo)
						AND (old.root<>fresh.first_root OR old.root<>fresh.last_root)
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
		// A paired root can retain different witness keys during a partial scan;
		// its files still belong to one scope and must never source each other.
		Statement sources(sql, "SELECT f.id,f.path,f.root,f.repo,f.device,f.inode,f.size,"
			" f.msec,f.mnsec,f.csec,f.cnsec,c.start FROM chunks c JOIN files f ON f.id=c.file"
			" WHERE c.hash=? AND c.length=? AND f.fsid=? AND f.repo<>? AND f.root<>? ORDER BY c.file,c.start");
		Statement consecutive(sql, R"SQL(
			SELECT d.start, d.length, s.hash=d.hash AND s.length=d.length AND NOT EXISTS (
				SELECT 1 FROM chunks earlier JOIN files f ON f.id=earlier.file
				WHERE earlier.hash=d.hash AND earlier.length=d.length AND earlier.file<=s.file
					AND (earlier.file<s.file OR earlier.start<s.start) AND f.fsid=?7 AND f.repo<>?8 AND f.root<>?9
			)
			FROM chunks d LEFT JOIN chunks s ON s.file=?1 AND s.start=?2+(d.start-?3)
			WHERE d.file=?4 AND d.start>=?5 AND d.start<?6 ORDER BY d.start
		)SQL");
		Statement done(sql, "UPDATE files SET pending=0 WHERE id=?");
		Statement record(sql, "INSERT INTO dedup_totals VALUES (?,?,?,1)"
			" ON CONFLICT(source,destination) DO UPDATE SET bytes=bytes+excluded.bytes, ranges=ranges+1");
		// This pass changes only pending flags and statistics. Indexed content,
		// paths and stamps remain fixed until the next scan, so the raw first row
		// is reusable; existing descriptor validation and kernel comparison remain unchanged.
		FirstSourceCache first_sources;

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
			const Fd dest = open_indexed(destination_path, root, repo, dest_stamp, policy, state, mounts);
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
					const auto hash = chunks.number(2);
					bool query_started = false;
					const auto &first = first_sources.get(sources, hash, length, fsid, repo, root, query_started);
					std::optional<SourceCandidate> fallback;
					auto next_candidate = [&]() -> const SourceCandidate * {
						if (!query_started) {
							sources.bind(1, hash).bind(2, length).bind(3, fsid).bind(4, repo).bind(5, root);
							query_started = true;
						}
						while (sources.next()) {
							// Resume the original order after the already attempted first
							// row, including further offsets in that same source file.
							if (sources.number(0) == first->id && sources.number(11) == first->offset) {
								continue;
							}
							fallback.emplace(sources);
							return &*fallback;
						}
						return nullptr;
					};
					for (const auto *candidate = first ? &*first : nullptr; !stopping && candidate;
						candidate = next_candidate()) {
						if (source_id != candidate->id) {
							source_id = candidate->id;
							source = open_indexed(candidate->path, candidate->root,
								candidate->repo, candidate->stamp, policy, state, mounts);
						}
						if (!source) {
							continue;
						}
						const auto source_offset = candidate->offset;
						const auto limit = std::min(max_dedup_size, dest_stamp.values[2] - offset);
						auto requested = length;
						if (length == static_cast<Integer>(chunk_size) && length < limit) {
							// Keep the first source for every chunk, including repeated
							// content within a file, so batching preserves space savings.
							consecutive.bind(1, source_id).bind(2, source_offset).bind(3, offset)
								.bind(4, id).bind(5, offset + length).bind(6, offset + limit)
								.bind(7, fsid).bind(8, repo).bind(9, root);
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
								record.bind(1, candidate->root).bind(2, root).bind(3, added).run();
								shared += added;
								const auto &source_path = candidate->path;
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
				"Discover Git/README projects, build/dependency directories, and named backups on mounted\n"
				"writable Btrfs filesystems and deduplicate ranges across scopes.\n"
				"Default system scans prioritize build caches, backups, opt applications, and user data.\n"
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
	const ScanPolicy policy(priority_roots, roots);
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
