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

// Walk `dir` and collect candidate source files, applying the same
// FilterPolicy rules as the scanner (skip dirs, gitignore,
// .codescopeignore, bundle suffixes, filename/suffix skips, language
// filter). Also ingests the project-root README as a knowledge
// document and runs the incremental scan-state gate.
int collectFileJobs(uint64_t project_id, const std::string &dir,
		    FilterPolicy &filter,
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
		for (auto &entry : it) {
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
			// .md files are in skip_suffixes_ (filter_policy.cpp:422)
			// so they never reach the source-code indexing path.
			// But the knowledge layer (CapabilityPlugin,
			// ContractPlugin) needs README content in the
			// document table to extract capabilities/contracts.
			// Therefore we intercept README.md here — BEFORE
			// shouldSkipEntry() drops it — and ingest it via
			// insertDocument().
			//
			// Only the project-root README is ingested as a
			// knowledge document; nested READMEs are ignored
			// to avoid noise from vendored deps.
			if (entry.is_regular_file()) {
				const std::string &fp = entry.path().string();
				std::string fname = fp;
				size_t sl = fp.find_last_of("/\\");
				if (sl != std::string::npos)
					fname = fp.substr(sl + 1);
				// Case-insensitive README.md match
				std::string fname_lower = fname;
				for (auto &c : fname_lower)
					c = static_cast<char>(std::tolower(c));

				// Check if this README is at project root
				bool is_root_readme = false;
				if (fname_lower == "readme.md" ||
				    fname_lower == "readme.markdown" ||
				    fname_lower == "readme") {
					// Project-root README: its parent dir == dir
					std::string parent = fp;
					size_t ps = parent.find_last_of("/\\");
					parent = (ps != std::string::npos) ?
							 parent.substr(0, ps) :
							 "";
					is_root_readme = (parent == dir);
				}

				if (is_root_readme) {
					// Ingest README content into document table.
					// type=0 (kDocumentTypeReadme) signals the
					// knowledge layer to parse capabilities.
					std::string content =
						readFile(fp.c_str());
					if (!content.empty()) {
						int doc_type =
							0; // kDocumentTypeReadme
						// insertDocument's 5th/6th params are
						// start_line / end_line (1-based line
						// numbers), NOT byte offsets. Count
						// newlines to compute the line range.
						int line_count = 1;
						for (char c : content)
							if (c == '\n')
								++line_count;
						if (!g_store->insertDocument(
							    project_id,
							    doc_type, fp,
							    content, 1,
							    line_count)) {
							fprintf(stderr,
								"engine: insertDocument failed for %s: %s "
								"[module=engine, method=collectFileJobs]\n",
								fp.c_str(),
								g_store->error()
									.c_str());
						}
					}
					// README is ingested as a document, NOT as
					// source code — skip the rest of the loop.
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
		err << "{\"ok\":false,\"error\":\"scan error: "
		    << jsonEscape(e.what()) << "\"}";
		err_json = err.str();
		return -1;
	}
	return 0;
}

} // namespace engine_index_discover
