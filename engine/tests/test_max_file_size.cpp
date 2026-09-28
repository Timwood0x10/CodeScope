/**
 * test_max_file_size: the documented CODESCOPE_MAX_FILE_SIZE default.
 *
 * README §3/§9 promise a 5 MB (5242880 byte) default cap, and files above it
 * are skipped silently. The cap is a hard-coded constant
 * (kMaxFileSize in engine_index_sched.h), so nothing but a test keeps the
 * documentation and the code from drifting apart again
 * (CODE_REVIEW_2026-09-27.md D2-1: the README claimed 10 MB in §3 and
 * "(unset)" in §9 while the code used 5 MB).
 *
 * Cases:
 *   1. Default (env unset): a 6 MB source file is skipped, a small one indexed.
 *   2. CODESCOPE_MAX_FILE_SIZE raised: the same big file is indexed.
 */

#include "../include/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static const char *kProjDir = "/tmp/test_max_file_size_proj";
static const char *kDbPath = "/tmp/test_max_file_size.db";

static void check(bool cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "\nFAIL: %s\n", msg);
		std::error_code ec;
		fs::remove_all(kProjDir, ec);
		fs::remove(kDbPath, ec);
		exit(1);
	}
}

static void write_file(const std::string &path, const std::string &content)
{
	FILE *f = fopen(path.c_str(), "w");
	check(f != nullptr, "fopen");
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);
}

/// A valid C file just over `bytes` in size: one large block comment plus a
/// tiny function. Comment-heavy keeps the parse cheap (the point is the size
/// gate, not parse performance), unlike thousands of duplicate declarations.
static std::string oversized_source(size_t bytes)
{
	std::string code = "/*";
	code.append(bytes, 'x');
	code += "*/\nint big_symbol(void) { return 0; }\n";
	return code;
}

/// Number of indexed source files reported by project_overview.
static int indexed_file_count(uint64_t pid)
{
	char *overview = engine_get_project_overview(pid);
	check(overview != nullptr, "project_overview result");
	const char *key = strstr(overview, "\"total_files\":");
	int files = -1;
	if (key != nullptr)
		files = atoi(key + strlen("\"total_files\":"));
	engine_free_string(overview);
	return files;
}

/// Index kProjDir into `db_path` and return the reported file count.
static int index_and_count(const char *db_path)
{
	unlink(db_path);
	check(engine_init(db_path) == 0, "engine_init");
	uint64_t pid = engine_create_project(kProjDir, "max-file-size");
	check(pid > 0, "create_project");

	char *idx = engine_index_project(pid, kProjDir, NULL);
	check(idx != nullptr, "index_project result");
	check(strstr(idx, "\"ok\":true") != nullptr, "index_project ok");
	engine_free_string(idx);

	int files = indexed_file_count(pid);
	engine_shutdown();
	return files;
}

int main()
{
	std::error_code ec;
	fs::remove_all(kProjDir, ec);
	fs::create_directories(kProjDir);
	fs::remove(kDbPath, ec);

	write_file(std::string(kProjDir) + "/small.c",
		   "int small_symbol(void) { return 0; }\n");
	// 6 MB: above the 5 MB (5242880 byte) default, below the raised cap below.
	write_file(std::string(kProjDir) + "/big.c",
		   oversized_source(6u * 1024 * 1024));

	// ── 1. Default cap (env unset) ───────────────────────────────
	unsetenv("CODESCOPE_MAX_FILE_SIZE");
	const int default_files = index_and_count(kDbPath);
	printf("default cap: indexed %d file(s)\n", default_files);
	check(default_files == 1,
	      "default 5 MB cap must index only the small file (big.c skipped)");

	// ── 2. Raised cap: the same big file is indexed ──────────────
	setenv("CODESCOPE_MAX_FILE_SIZE", "10000000", 1);
	const int raised_files = index_and_count(kDbPath);
	printf("raised cap: indexed %d file(s)\n", raised_files);
	check(raised_files == 2,
	      "CODESCOPE_MAX_FILE_SIZE=10000000 must include the 6 MB file");
	unsetenv("CODESCOPE_MAX_FILE_SIZE");

	fs::remove_all(kProjDir, ec);
	fs::remove(kDbPath, ec);
	printf("\n=== max file size test passed ===\n");
	return 0;
}
