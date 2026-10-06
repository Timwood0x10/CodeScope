#include "util/json_writer.h"
#include "engine_index_discover.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

#include "engine_internal.h"
#include "posix_compat.h"

namespace engine_index_discover
{

// ── README / knowledge documents ────────────────────────────────────────
//
// One implementation for every caller: the discovery walk below and the
// enhance pass (engine_enhance_project) both route through these helpers, so
// the accepted names, the replace-not-append rule and the log tags cannot
// drift apart between the paths that ingest READMEs.

namespace
{

/// README names the engine ingests, lower-cased. Kept in one place so the walk
/// and the project-root lookup cannot disagree (code_rules §5: no duplicated
/// literals).
const char *const kReadmeNames[] = { "readme.md", "readme.markdown", "readme" };

/// @param name File name to normalise.
/// @return The name lower-cased (ASCII; file names here are ASCII in practice).
std::string toLowerName(const std::string &name)
{
	std::string lower = name;
	for (auto &c : lower)
		c = static_cast<char>(
			std::tolower(static_cast<unsigned char>(c)));
	return lower;
}

/// @param file_name Base name without any directory part.
/// @return true when it is one of the README names the engine ingests.
bool isReadmeName(const std::string &file_name)
{
	const std::string lower = toLowerName(file_name);
	for (const char *candidate : kReadmeNames) {
		if (lower == candidate)
			return true;
	}
	return false;
}

/// @param path Absolute path.
/// @return The part after the last path separator.
std::string baseName(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// @param path Absolute path.
/// @return The directory containing `path`, or an empty string when it has none.
std::string parentDir(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() :
					    path.substr(0, slash);
}

/// @param content File body.
/// @return 1-based line count, which insertDocument records as `end_line`.
int countLines(const std::string &content)
{
	int lines = 1;
	for (char c : content)
		if (c == '\n')
			++lines;
	return lines;
}

} // namespace

bool ingestReadmeDocument(EngineContext *ctx, uint64_t project_id,
			  const std::string &readme_path)
{
	if (!ctx || !ctx->store)
		return false;

	const std::string content = readFile(readme_path.c_str());
	if (content.empty())
		return false;

	// type 0 (kDocumentTypeReadme) signals the knowledge layer to parse
	// capabilities out of the content.
	const int doc_type = 0;
	// Replace, never append: the drift tools concatenate every README row of a
	// project, so a duplicate would silently double the weight of its claims.
	if (!ctx->store->deleteDocument(project_id, doc_type, readme_path)) {
		fprintf(stderr,
			"engine: deleteDocument failed for %s: %s "
			"[module=engine, method=ingestReadmeDocument]\n",
			readme_path.c_str(), ctx->store->error().c_str());
		return false;
	}
	if (!ctx->store->insertDocument(project_id, doc_type, readme_path,
					content, 1, countLines(content))) {
		fprintf(stderr,
			"engine: insertDocument failed for %s: %s "
			"[module=engine, method=ingestReadmeDocument]\n",
			readme_path.c_str(), ctx->store->error().c_str());
		return false;
	}
	return true;
}

int ingestProjectRootReadme(EngineContext *ctx, uint64_t project_id)
{
	if (!ctx || !ctx->store)
		return -1;

	const std::string root = ctx->store->getProjectRootPath(project_id);
	if (root.empty()) {
		fprintf(stderr,
			"engine: project %llu has no root path; cannot ingest its README "
			"[module=engine, method=ingestProjectRootReadme]\n",
			(unsigned long long)project_id);
		return -1;
	}

	// List the root instead of guessing name casing: a directory listing with a
	// case-insensitive comparison also catches README.MD / Readme.md, which a
	// case-sensitive probe would miss on Linux.
	std::error_code ec;
	std::filesystem::directory_iterator it(
		root,
		std::filesystem::directory_options::skip_permission_denied, ec);
	if (ec) {
		fprintf(stderr,
			"engine: cannot list project root %s: %s "
			"[module=engine, method=ingestProjectRootReadme]\n",
			root.c_str(), ec.message().c_str());
		return -1;
	}
	const std::filesystem::directory_iterator end;
	for (; it != end; it.increment(ec)) {
		if (ec)
			break;
		std::error_code entry_ec;
		if (!it->is_regular_file(entry_ec))
			continue;
		const std::string name = it->path().filename().string();
		if (!isReadmeName(name))
			continue;
		return ingestReadmeDocument(ctx, project_id,
					    it->path().string()) ?
			       1 :
			       -1;
	}
	return 0;
}

// Walk `dir` and collect candidate source files, applying the same
// FilterPolicy rules as the scanner (skip dirs, gitignore,
// .codescopeignore, bundle suffixes, filename/suffix skips, language
// filter). Also ingests the project-root README as a knowledge
// document and runs the incremental scan-state gate.
int collectFileJobs(EngineContext *ctx, uint64_t project_id,
		    const std::string &dir, FilterPolicy &filter,
		    const std::unordered_set<std::string> &scan_state,
		    std::vector<FileJob> &jobs, bool &is_reindex,
		    std::string &err_json)
{
	// Pre-detect Java projects BEFORE the directory walk. The FilterPolicy
	// Java carve-out defers test/docs/example/samples/... dirs to a
	// top-only check ONLY when lang_context_ == "java", but lang_context_
	// previously flipped only upon seeing the FIRST .java file during the
	// walk — and that file may itself live under an example/samples/...
	// dir which is skipped at any depth while lang_context_ is still
	// empty. That chicken-and-egg made Java projects with such package
	// dirs index 0 files (e.g. spring-petclinic's
	// org/springframework/samples/petclinic). Fix: cheap recursive scan
	// for any *.java before the main walk and flip lang_context_ early.
	{
		std::error_code ec;
		auto pit = std::filesystem::recursive_directory_iterator(
			dir,
			std::filesystem::directory_options::skip_permission_denied,
			ec);
		std::filesystem::recursive_directory_iterator pend;
		while (!ec && pit != pend) {
			const auto &pent = *pit;
			if (pent.is_regular_file() &&
			    pent.path().extension() == ".java") {
				filter.setLangContext("java");
				break;
			}
			pit.increment(ec);
		}
	}

	try {
		// P0-2: standalone discovery timing. Previously this phase only
		// reported entry counts (seen_dirs/skipped_files/...); wall-clock
		// cost was invisible. Instrument the full walk so discovery can
		// be compared against parse/buildGraph stages.
		using namespace std::chrono;
		auto t_discovery_start = steady_clock::now();
		auto it = std::filesystem::recursive_directory_iterator(
			dir, std::filesystem::directory_options::
				     skip_permission_denied);
		// An explicit iterator loop, NOT `for (auto &entry : it)`.
		//
		// A range-for over an iterator copies it: `begin()` returns the
		// iterator by value, the loop advances that copy, and the pruning
		// call below would then operate on the original object that nobody
		// is iterating. The result was measured, not assumed: with
		// `.gitignore` holding `build-x/`, the walk reported
		// `skipped_dirs=4` and STILL descended into all four levels,
		// because `disable_recursion_pending()` was applied to a copy.
		// Directory pruning was therefore cosmetic for every entry point
		// that relies on it — the hard-skip names and `.gitignore` rules
		// both counted their matches and then walked in anyway.
		const auto it_end =
			std::filesystem::recursive_directory_iterator();
		for (; it != it_end; ++it) {
			const auto &entry = *it;
			// seen_dirs counts ONLY directory entries — recursive_
			// directory_iterator yields files too, so counting every
			// entry here inflated the metric with file visits. JSON
			// discovery.seen_dirs and the discovery= log share this
			// counter, so both now report true directory counts.
			if (entry.is_directory())
				filter.stats().seen_dirs++;
			std::string rel = entry.path().string();
			if (rel.size() > dir.size() + 1)
				rel = rel.substr(dir.size() + 1);
			else
				rel.clear();

			// ── README / document ingestion (BEFORE skip filter) ──
			// .md files are in skip_suffixes_ (filter_policy.cpp:422) so they
			// never reach the source-code indexing path, but the knowledge
			// layer (CapabilityPlugin, ContractPlugin) and the drift tools need
			// README content in the document table. Intercept the README at the
			// root of the directory being scanned — BEFORE shouldSkipEntry()
			// drops it — and ingest it through the shared helper.
			//
			// A README at a nested directory's root is picked up when that
			// directory is scanned; the project root itself is additionally
			// covered by engine_enhance_project (ingestProjectRootReadme), which
			// every index path runs.
			if (entry.is_regular_file()) {
				const std::string readme_path =
					entry.path().string();
				if (isReadmeName(baseName(readme_path)) &&
				    parentDir(readme_path) == dir) {
					ingestReadmeDocument(ctx, project_id,
							     readme_path);
					// A README is a knowledge document, never source code.
					continue;
				}
			}

			if (!rel.empty()) {
				bool entry_is_dir = entry.is_directory();
				// Use the consolidated entry check (single source of
				// truth) so the indexer and scanner apply identical
				// filtering: skip_dirs (any depth), gitignore,
				// .codescopeignore, bundle-dir suffixes, filename skip,
				// filename-prefix skip, and suffix skip.
				if (filter.shouldSkipEntry(rel, entry_is_dir)) {
					if (entry_is_dir) {
						it.disable_recursion_pending();
						filter.stats().skipped_dirs++;
					} else {
						filter.stats().skipped_files++;
					}
					continue;
				}
			}
			if (entry.is_regular_file()) {
				filter.stats().seen_files++;

				// Incremental: check file_scan_state to skip unchanged files
				struct stat file_stat;
				int64_t mtime = 0, fsize = 0;
				bool file_unchanged = false;
				if (stat(entry.path().string().c_str(),
					 &file_stat) == 0) {
					mtime = static_cast<int64_t>(
						file_stat.st_mtime);
					fsize = static_cast<int64_t>(
						file_stat.st_size);
					// O(1) in-memory lookup instead of per-file DB query.
					// M2: two-stage incremental gate. Stage 1 is the cheap
					// mtime|size gate (no file read). Only when it matches do
					// we hash the file and check the mtime|size|hash gate,
					// closing the "same size + same mtime but changed content"
					// hole. Files whose mtime/size differ skip without being
					// read, so incremental performance is preserved.
					std::string base =
						entry.path().string() + "|" +
						std::to_string(mtime) + "|" +
						std::to_string(fsize);
					if (scan_state.count(base) > 0) {
						std::string ch = fileContentHash(
							entry.path()
								.string()
								.c_str());
						if (!ch.empty())
							file_unchanged =
								scan_state.count(
									base +
									"|" +
									ch) > 0;
						// If hashing failed (unreadable), fall back to
						// treating as unchanged on the mtime|size gate.
						else
							file_unchanged = true;
					}
				}
				if (file_unchanged) {
					is_reindex = true;
					filter.stats().skipped_files++;
					continue;
				}
				const char *lang = filter.detectLanguage(
					entry.path().string().c_str());
				if (!lang) {
					filter.stats().skipped_lang++;
					continue;
				}
				// Detect Java projects on the fly — the FIRST .java
				// file flips filter into Java mode so test/docs/samples
				// collisions with Java package namespaces (e.g.
				// org/springframework/samples/petclinic) get the
				// top-only (depth ≤ 3) treatment instead of being
				// skipped at any depth. See README.md "Why Java is
				// the (only) exception". Idempotent — setLangContext
				// is cheap and safe to repeat.
				if (strcmp(lang, "java") == 0 &&
				    filter.langContext() != "java") {
					filter.setLangContext("java");
				}
				if (!filter.isLanguageAccepted(lang)) {
					filter.stats().skipped_lang++;
					continue;
				}
				filter.stats().candidate_files++;
				auto file_size =
					entry.is_regular_file() ?
						std::filesystem::file_size(
							entry.path()) :
						0;
				jobs.push_back({ entry.path().string(), lang,
						 file_size });
			}
		}
		// P0-2: report the standalone discovery wall-clock. Same
		// [module=engine, method=...] format as the other pipeline
		// stages so it can be parsed by the same tooling.
		auto discovery_ms = duration_cast<milliseconds>(
			steady_clock::now() - t_discovery_start);
		fprintf(stderr,
			"engine: discovery=%lldms (seen_dirs=%llu seen_files=%llu "
			"skipped_dirs=%llu skipped_files=%llu candidate_files=%zu) "
			"[module=engine, method=collectFileJobs]\n",
			static_cast<long long>(discovery_ms.count()),
			static_cast<unsigned long long>(
				filter.stats().seen_dirs),
			static_cast<unsigned long long>(
				filter.stats().seen_files),
			static_cast<unsigned long long>(
				filter.stats().skipped_dirs),
			static_cast<unsigned long long>(
				filter.stats().skipped_files),
			jobs.size());
	} catch (const std::exception &e) {
		std::ostringstream err;
		// The helper adds the [module=…, method=…] trace chain the hand-built
		// envelope was missing (plan/rules/code_rules.md: every error must be
		// traceable to a module and a method).
		err_json = util::okFalseEnvelope("ffi", "engine_index_discover",
						 std::string("scan error: ") +
							 e.what());
		return -1;
	}
	return 0;
}

} // namespace engine_index_discover
