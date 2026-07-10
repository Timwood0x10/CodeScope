#include "engine_internal.h"
#include "filter_policy.h"
#include "platform_win.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h> // waitpid, WIFEXITED, WEXITSTATUS, WTERMSIG
#include <unistd.h> // pipe, fork, execvp, dup2, read, close
#endif

// Phase A scanner removed — redirected to engine_index_project with fast mode.
// ─── engine_scan_project ──────────────────────────────────────

// ─── Phase A: engine_scan_project ──────────────────────────────
// Redirected to engine_index_project with fast mode.
// Regex scan eliminated — tree-sitter is fast enough (727ms for 150K lines).

char *engine_scan_project(uint64_t project_id, const char *dir_path,
			  const char *language_filter)
{
	if (!g_store)
		return dupString("{\"error\":\"engine not initialized\"}");
	(void)language_filter;
	return engine_index_project(project_id, dir_path, nullptr);
}
