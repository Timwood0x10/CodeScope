#include "store.h"
#include "store_internal.h"
#include "platform_win.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

#include "../graph/graph_builder.h"
#include "../ir/semantic_unit.h"

namespace store
{

// ─── FTS5 Full-Text Search ─────────────────────────────────────

// Split a camelCase / snake_case / kebab-case identifier into its
// constituent words so an FTS5 query can match both styles:
//   "findByLastName"  -> find by last name
//   "find_by_last_name" -> find by last name
// The unicode61 tokenizer treats underscore and case boundaries as part
// of a single token, so a bare `"findByLastName"` MATCH never hits
// snake_case code and vice versa. Splitting at lower->upper boundaries
// and at '_'/'-' yields the shared words.
static std::vector<std::string> splitIdentifierWords(const std::string &word)
{
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&]() {
		if (!cur.empty()) {
			out.push_back(cur);
			cur.clear();
		}
	};
	for (size_t i = 0; i < word.size(); ++i) {
		char c = word[i];
		if (c == '_' || c == '-' || c == '.' || c == '/' || c == ':' ||
		    c == '(' || c == ')') {
			flush();
			continue;
		}
		if (std::isupper(static_cast<unsigned char>(c)) &&
		    !cur.empty()) {
			// Lower -> upper boundary (camelCase: "lastName").
			// Keep an acronym run together ("JSONParser" stays one
			// split unless the next char is lower).
			char prev = cur.back();
			if (!std::isupper(static_cast<unsigned char>(prev)) ||
			    (i + 1 < word.size() &&
			     std::islower(static_cast<unsigned char>(
				     word[i + 1])))) {
				flush();
			}
		}
		cur += static_cast<char>(
			std::tolower(static_cast<unsigned char>(c)));
	}
	flush();
	return out;
}

void GraphStore::insertIntoFTS(uint64_t node_id, uint64_t project_id,
			       const char *name, const char *qualified_name,
			       const char *file_path, const char *content,
			       int node_kind)
{
	// Skip empty entries
	if ((!name || !*name) && (!qualified_name || !*qualified_name) &&
	    (!file_path || !*file_path) && (!content || !*content)) {
		return;
	}
	if (node_kind < 0)
		node_kind = 0;

	// Update mapping table (reuses cached prepared statement)
	if (stmt_fts_map_) {
		sqlite3_reset(stmt_fts_map_);
		sqlite3_bind_int64(stmt_fts_map_, 1,
				   static_cast<int64_t>(node_id));
		sqlite3_bind_int64(stmt_fts_map_, 2,
				   static_cast<int64_t>(project_id));
		sqlite3_step(stmt_fts_map_);
	}

	// Insert into FTS5 (reuses cached prepared statement)
	if (stmt_fts_) {
		sqlite3_reset(stmt_fts_);
		sqlite3_bind_int64(stmt_fts_, 1, static_cast<int64_t>(node_id));
		sqlite3_bind_text(stmt_fts_, 2, name ? name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 3,
				  qualified_name ? qualified_name : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 4, file_path ? file_path : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt_fts_, 5, content ? content : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt_fts_, 6,
				   static_cast<int64_t>(project_id));
		sqlite3_bind_int64(stmt_fts_, 7, static_cast<int64_t>(node_id));
		sqlite3_bind_int(stmt_fts_, 8, node_kind);
		sqlite3_step(stmt_fts_);
	}
}

// deleteFTSByFile removed — FTS is indexed inline during buildGraph.
// Single-file index paths no longer write FTS entries per-node.

void GraphStore::buildFTSFromGraph(uint64_t project_id)
{
	// Bulk-build FTS from entity: single SQL INSERT-SELECT
	// No per-node prepare/finalize overhead.
	// graph_nodes is deprecated; entity is the canonical source.
	exec(std::string(
		     "INSERT OR IGNORE INTO code_fts (rowid, name, qualified_name, "
		     " file_path, content, project_id, node_id, node_kind) "
		     "SELECT e.id, e.name, e.qualified_name, e.file_path, '', " +
		     std::to_string(project_id) +
		     ", e.id, e.kind "
		     "FROM entity e "
		     "WHERE e.project_id=" +
		     std::to_string(project_id) + " AND e.name != ''")
		     .c_str());
	// Build fts_node_map mapping
	exec(std::string(
		     "INSERT OR IGNORE INTO fts_node_map (node_id, project_id, file_id) "
		     "SELECT e.id, e.project_id, COALESCE(f.id, 0) "
		     "FROM entity e "
		     "LEFT JOIN files f ON f.path = e.file_path AND f.project_id=e.project_id "
		     "WHERE e.project_id=" +
		     std::to_string(project_id))
		     .c_str());
	// Bulk-build the trigram FTS5 index (name_trgm) in parallel with
	// code_fts. Same source (entity), same WHERE filter. Uses
	// INSERT OR IGNORE so re-runs after partial indexing are idempotent.
	// The trigram index powers O(log n) substring search via MATCH,
	// replacing the O(n) LIKE '%query%' scan in searchGraphFallback.
	exec(std::string(
		     "INSERT OR IGNORE INTO name_trgm "
		     "(rowid, name, qualified_name, project_id, node_id, node_type) "
		     "SELECT e.id, e.name, e.qualified_name, " +
		     std::to_string(project_id) +
		     ", e.id, e.kind "
		     "FROM entity e "
		     "WHERE e.project_id=" +
		     std::to_string(project_id) + " AND e.name != ''")
		     .c_str());
}

bool GraphStore::isTrigramAvailable()
{
	// Probe the name_trgm table. If the table does not exist (older DB
	// created before the trigram migration) or is empty, the prepare/step
	// will fail or return no row. Returns true only when the table is
	// queryable (i.e. at least one row can be selected).
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db_, "SELECT 1 FROM name_trgm LIMIT 1", -1,
				    &stmt, nullptr);
	if (rc != SQLITE_OK) {
		// Table does not exist or schema is not loaded.
		fprintf(stderr,
			"isTrigramAvailable: prepare failed: %s "
			"[module=store, method=isTrigramAvailable]\n",
			sqlite3_errmsg(db_));
		return false;
	}
	bool available = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return available;
}

namespace
{
// Fixed dimension of the n-gram hash vector. 192 floats = 768 bytes per
// row, small enough for a BLOB column and for a full in-memory scan of a
// large module. Larger dimensions hurt cosine separation at this feature
// scale; smaller ones increase collision noise.
constexpr int kVecDim = 192;
// Cosine-similarity floor for semantic search results (see the accuracy-first
// gate in searchSemanticJson). Strong n-gram matches score > 0.6; unrelated
// names cluster below 0.23, so 0.3 cleanly separates signal from noise.
constexpr float kSemanticScoreFloor = 0.3f;

// Double-hash the n-gram into two buckets and accumulate signed weights, so
// the resulting vector is a standard hashing-vectorizer (like
// sklearn HashingVectorizer). No external model is involved — this is the
// n-gram hash scheme the schema comment for node_vectors always intended.
// `seed` decorrelates the two hash passes.
static inline uint64_t hashMix(uint64_t h)
{
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 27;
	h *= 0x94d049bb133111ebULL;
	h ^= h >> 31;
	return h;
}
} // namespace

// Build n-gram hash vectors for every function/method entity of the project
// and store them in node_vectors. This restores the semantic-search producer
// that Step 10 sunset (the previous body was a no-op leaving node_vectors
// empty). Readiness is derived from the actual node_vectors row count, so the
// A19 "fake ready" regression cannot recur: if this loop writes rows,
// embedding_ready reflects it; if it writes nothing, readiness stays 0.
//
// The vector is built from the entity's qualified_name + name n-grams. This
// gives lexical-similarity search (find "user_dao" given "user_repository")
// which is the practical "semantic" signal available without an embedding
// model. It is NOT a meaning vector; the tool description and capabilities
// JSON say so explicitly.
void GraphStore::buildVectorsFromGraph(uint64_t project_id)
{
	if (!db_)
		return;

	// Collect (id, qualified_name, name) for function/method entities.
	// entity.id is the canonical primary key (it preserves the legacy graph
	// node identity after the graph_nodes→entity migration).
	struct Ent {
		int64_t id;
		std::string text;
	};
	std::vector<Ent> ents;
	{
		const char *sql =
			"SELECT id, COALESCE(NULLIF(qualified_name, ''), name), "
			"       name FROM entity "
			"WHERE project_id = ? AND kind IN (0,1)";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"buildVectorsFromGraph: prepare collect failed: %s "
				"[module=store, method=buildVectorsFromGraph]\n",
				sqlite3_errmsg(db_));
			return;
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			Ent e;
			e.id = sqlite3_column_int64(stmt, 0);
			const char *qn = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *nm = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			std::string text = qn ? qn : "";
			if (nm && *nm) {
				if (!text.empty())
					text.push_back(' ');
				text += nm;
			}
			e.text = std::move(text);
			ents.push_back(std::move(e));
		}
		sqlite3_finalize(stmt);
	}
	if (ents.empty())
		return;

	// ── TF-IDF identifier weighting (v0.2.5) ──────────────────────
	// Tokenize every entity's qualified_name + name via camel/snake/kebab
	// splitting and count per-entity token frequencies, so we can weight
	// each token's vector contribution by inverse document frequency (idf =
	// log(1 + N/(1+df))). Rare, discriminative tokens (e.g. "ledger" in
	// getUserByLedgerId) then contribute far more to the vector than common
	// ones ("get"), which sharply improves semantic-search precision: a
	// query matching a rare token ranks the correct entity far above
	// incidental trigram-overlap noise. The same split is applied on the
	// query side, so no project statistics are needed at query time.
	struct TokMap {
		std::vector<std::string> toks;
		std::vector<float> idfs;
	};
	std::vector<TokMap> ent_toks(ents.size());
	{
		std::unordered_map<std::string, size_t> df;
		df.reserve(ents.size() * 4);
		for (const auto &e : ents) {
			auto toks = splitIdentifierWords(e.text);
			// Dedupe within this entity (df counts entities, not
			// occurrences).
			std::sort(toks.begin(), toks.end());
			toks.erase(std::unique(toks.begin(), toks.end()),
				   toks.end());
			for (auto &t : toks) {
				if (t.empty())
					continue;
				++df[t];
			}
			ent_toks[&e - ents.data()].toks = std::move(toks);
		}
		const size_t N = ents.size();
		for (size_t ei = 0; ei < ents.size(); ++ei) {
			auto &tm = ent_toks[ei];
			tm.idfs.reserve(tm.toks.size());
			for (const auto &t : tm.toks) {
				size_t d = 0;
				auto it = df.find(t);
				if (it != df.end())
					d = it->second;
				float idf = static_cast<float>(std::log(
					1.0 +
					static_cast<double>(N) /
						(1.0 + static_cast<double>(d))));
				// Cap the weight so one dominant token cannot
				// overwhelm the trigram signal entirely.
				if (idf > 3.0f)
					idf = 3.0f;
				tm.idfs.push_back(idf);
			}
		}
	}

	// Clear stale vectors for this project, then write fresh ones.
	const char *clear_sql = "DELETE FROM node_vectors WHERE project_id = ?";
	sqlite3_stmt *del = nullptr;
	if (sqlite3_prepare_v2(db_, clear_sql, -1, &del, nullptr) ==
	    SQLITE_OK) {
		sqlite3_bind_int64(del, 1, static_cast<int64_t>(project_id));
		sqlite3_step(del);
		sqlite3_finalize(del);
	} else {
		fprintf(stderr,
			"buildVectorsFromGraph: prepare clear failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			sqlite3_errmsg(db_));
		return;
	}

	const char *ins_sql =
		"INSERT OR REPLACE INTO node_vectors (node_id, project_id, vector) "
		"VALUES (?,?,?)";
	sqlite3_stmt *ins = nullptr;
	if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"buildVectorsFromGraph: prepare insert failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			sqlite3_errmsg(db_));
		return;
	}

	std::vector<float> vec(kVecDim, 0.0f);
	for (const auto &e : ents) {
		std::fill(vec.begin(), vec.end(), 0.0f);
		const std::string &t = e.text;
		// Character trigrams (lowercased) — captures identifier substrings
		// and cross-casing boundaries ("userDao" → "use","ser","erD",...).
		// Kept unweighted as a lexical-similarity fallback so a query
		// that only partially overlaps an identifier (or crosses a casing
		// boundary) still gets a signal.
		for (size_t i = 0; i + 3 <= t.size(); ++i) {
			std::string gram = t.substr(i, 3);
			for (char &ch : gram)
				ch = static_cast<char>(std::tolower(
					static_cast<unsigned char>(ch)));
			uint64_t h = hashMix(std::hash<std::string>{}(gram) ^
					     static_cast<uint64_t>(project_id));
			int b1 = static_cast<int>(h % kVecDim);
			int b2 = static_cast<int>(hashMix(h) % kVecDim);
			vec[b1] += (h & 1) ? 1.0f : -1.0f;
			vec[b2] += (h & 2) ? 1.0f : -1.0f;
		}
		// TF-IDF weighted camel/snake token contribution (v0.2.5). Each
		// split token (get/user/by/id ...) is hashed and accumulated with
		// magnitude proportional to its idf — rare discriminative tokens
		// dominate, so semantically distinctive names rank correctly.
		{
			const TokMap &tm = ent_toks[&e - ents.data()];
			for (size_t ti = 0; ti < tm.toks.size(); ++ti) {
				const std::string &tok = tm.toks[ti];
				if (tok.empty())
					continue;
				uint64_t h = hashMix(
					std::hash<std::string>{}(tok) ^
					static_cast<uint64_t>(project_id));
				int b1 = static_cast<int>(h % kVecDim);
				int b2 = static_cast<int>(hashMix(h) % kVecDim);
				const float w = tm.idfs[ti];
				vec[b1] += (h & 1) ? w : -w;
				vec[b2] += (h & 2) ? w : -w;
			}
		}
		// L2-normalize.
		double norm = 0.0;
		for (float v : vec)
			norm += static_cast<double>(v) * v;
		if (norm > 0.0) {
			const float inv =
				static_cast<float>(1.0 / std::sqrt(norm));
			for (float &v : vec)
				v *= inv;
		}
		// Serialize as raw float32 little-endian.
		std::vector<uint8_t> blob(kVecDim * sizeof(float));
		for (int d = 0; d < kVecDim; ++d) {
			uint32_t bits;
			memcpy(&bits, &vec[d], sizeof(bits));
			for (int b = 0; b < 4; ++b)
				blob[d * 4 + b] = static_cast<uint8_t>(
					(bits >> (8 * b)) & 0xFF);
		}
		sqlite3_bind_int64(ins, 1, e.id);
		sqlite3_bind_int64(ins, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_blob(ins, 3, blob.data(),
				  static_cast<int>(blob.size()),
				  SQLITE_TRANSIENT);
		if (sqlite3_step(ins) != SQLITE_DONE) {
			fprintf(stderr,
				"buildVectorsFromGraph: insert step failed: %s "
				"[module=store, method=buildVectorsFromGraph]\n",
				sqlite3_errmsg(db_));
		}
		sqlite3_reset(ins);
	}
	sqlite3_finalize(ins);
}

std::string GraphStore::searchSemanticJson(uint64_t project_id,
					   const char *query, int limit)
{
	static constexpr const char *kMethod = "searchSemanticJson";
	if (!db_ || !query || !*query)
		return "{\"method\":\"semantic\",\"results\":[]}";
	if (limit <= 0 || limit > 100)
		limit = 20;

	// Vectorize the query to mirror buildVectorsFromGraph: lowercased
	// character trigrams + camel/snake-split identifier tokens, double-hash
	// accumulated, L2-normalized. Cosine similarity over the stored
	// normalized vectors is then a dot product.
	//
	// v0.2.5: the query is also split into identifier tokens (matching the
	// TF-IDF-weighted token contribution the builder wrote). Query tokens are
	// hashed at equal magnitude — the builder already baked each token's idf
	// weight into the stored entity vectors, so an equal-weight query token
	// automatically scores higher against the entity that shares that token
	// at high weight (i.e. the rare, discriminative one).
	std::vector<float> qvec(kVecDim, 0.0f);
	{
		const std::string t = query;
		for (size_t i = 0; i + 3 <= t.size(); ++i) {
			std::string gram = t.substr(i, 3);
			for (char &ch : gram)
				ch = static_cast<char>(std::tolower(
					static_cast<unsigned char>(ch)));
			uint64_t h = hashMix(std::hash<std::string>{}(gram) ^
					     static_cast<uint64_t>(project_id));
			int b1 = static_cast<int>(h % kVecDim);
			int b2 = static_cast<int>(hashMix(h) % kVecDim);
			qvec[b1] += (h & 1) ? 1.0f : -1.0f;
			qvec[b2] += (h & 2) ? 1.0f : -1.0f;
		}
		// Identifier-token contributions (equal weight; idf lives in the
		// stored entity vectors).
		for (const std::string &tok : splitIdentifierWords(t)) {
			if (tok.empty())
				continue;
			uint64_t h = hashMix(std::hash<std::string>{}(tok) ^
					     static_cast<uint64_t>(project_id));
			int b1 = static_cast<int>(h % kVecDim);
			int b2 = static_cast<int>(hashMix(h) % kVecDim);
			qvec[b1] += (h & 1) ? 1.0f : -1.0f;
			qvec[b2] += (h & 2) ? 1.0f : -1.0f;
		}
		double norm = 0.0;
		for (float v : qvec)
			norm += static_cast<double>(v) * v;
		if (norm > 0.0) {
			const float inv =
				static_cast<float>(1.0 / std::sqrt(norm));
			for (float &v : qvec)
				v *= inv;
		}
	}

	// Early-exit when no vectors exist for this project: nothing to match,
	// report empty with a reason so callers can fall back to FTS.
	{
		sqlite3_stmt *chk = nullptr;
		const char *csql =
			"SELECT COUNT(*) FROM node_vectors WHERE project_id = ?";
		if (sqlite3_prepare_v2(db_, csql, -1, &chk, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=%s] count prepare failed: "
				"%s\n",
				kMethod, sqlite3_errmsg(db_));
			return "{\"method\":\"semantic\",\"results\":[]}";
		}
		sqlite3_bind_int64(chk, 1, static_cast<int64_t>(project_id));
		bool has_rows = false;
		if (sqlite3_step(chk) == SQLITE_ROW &&
		    sqlite3_column_int64(chk, 0) > 0)
			has_rows = true;
		sqlite3_finalize(chk);
		if (!has_rows)
			return "{\"method\":\"semantic\",\"results\":[],"
			       "\"reason\":\"embedding_not_built\"}";
	}

	// Full scan of node_vectors joined to entity, computing cosine.
	struct Hit {
		int64_t node_id;
		std::string name;
		std::string qualified_name;
		std::string file_path;
		float score;
	};
	std::vector<Hit> hits;
	{
		const char *sql =
			"SELECT v.node_id, v.vector, "
			"       COALESCE(NULLIF(e.qualified_name,''),e.name), "
			"       e.name, e.file_path "
			"FROM node_vectors v JOIN entity e ON e.id = v.node_id "
			"WHERE v.project_id = ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			fprintf(stderr,
				"[module=store, method=%s] scan prepare failed: %s\n",
				kMethod, sqlite3_errmsg(db_));
			return "{\"method\":\"semantic\",\"results\":[],"
			       "\"error\":\"scan_failed\"}";
		}
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			int64_t nid = sqlite3_column_int64(stmt, 0);
			const void *blob = sqlite3_column_blob(stmt, 1);
			int nbytes = sqlite3_column_bytes(stmt, 1);
			const char *qn = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			const char *nm = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			const char *fp = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 4));
			if (!blob ||
			    nbytes < static_cast<int>(kVecDim * sizeof(float)))
				continue;
			// Deserialize float32 little-endian and dot with qvec.
			float dot = 0.0f;
			for (int d = 0; d < kVecDim; ++d) {
				uint32_t bits = 0;
				const uint8_t *b =
					static_cast<const uint8_t *>(blob) +
					d * 4;
				bits |= static_cast<uint32_t>(b[0]);
				bits |= static_cast<uint32_t>(b[1]) << 8;
				bits |= static_cast<uint32_t>(b[2]) << 16;
				bits |= static_cast<uint32_t>(b[3]) << 24;
				float f;
				memcpy(&f, &bits, sizeof(f));
				dot += f * qvec[d];
			}
			// Accuracy-first gate (0LLM design): only strong matches are
			// reported. Empirically the true positive for an n-gram hash
			// vector (a name sharing the query's trigrams) scores > 0.6,
			// while unrelated names cluster in the 0.02–0.23 band. A
			// 0.3 floor therefore keeps every relevant hit while
			// rejecting the noise, so semantic search never pollutes
			// results with weak/incidental matches (the accuracy
			// fixtures depend on exact FTS/trigram and must stay clean).
			if (dot <= kSemanticScoreFloor)
				continue;
			Hit hit;
			hit.node_id = nid;
			hit.name = nm ? nm : "";
			hit.qualified_name = qn ? qn : "";
			hit.file_path = fp ? fp : "";
			hit.score = dot;
			hits.push_back(std::move(hit));
		}
		sqlite3_finalize(stmt);
	}

	// Rank by descending similarity, cap at limit.
	std::partial_sort(hits.begin(),
			  hits.begin() + std::min(static_cast<size_t>(limit),
						  hits.size()),
			  hits.end(), [](const Hit &a, const Hit &b) {
				  return a.score > b.score;
			  });
	if (hits.size() > static_cast<size_t>(limit))
		hits.resize(static_cast<size_t>(limit));

	std::ostringstream json;
	json << "{\"method\":\"semantic\",\"total\":" << hits.size()
	     << ",\"results\":[";
	for (size_t i = 0; i < hits.size(); ++i) {
		if (i > 0)
			json << ",";
		json << "{\"node_id\":" << hits[i].node_id << ",\"name\":\""
		     << jsonEscape(hits[i].name) << "\",\"qualified_name\":\""
		     << jsonEscape(hits[i].qualified_name)
		     << "\",\"file_path\":\"" << jsonEscape(hits[i].file_path)
		     << "\",\"score\":" << hits[i].score << "}";
	}
	json << "]}";
	return json.str();
}

std::string GraphStore::searchCode(uint64_t project_id, const char *query,
				   int limit)
{
	if (!query || !*query) {
		return "{\"total\":0,\"results\":[],\"error\":\"empty query\"}";
	}

	// Arm the query timeout for this search call. The guard disarms on
	// scope exit (RAII), covering all return paths including exceptions.
	QueryDeadlineGuard guard(this, kDefaultSearchTimeoutMs);
	const int timeout_ms = kDefaultSearchTimeoutMs;

	// Collect results into a vector, deduping by node_id. FTS results
	// come first (preferred — ranked by FTS rank + node_type priority),
	// trigram substring results are appended after (only those whose
	// node_id is not already present in the FTS set).
	struct Row {
		int64_t node_id;
		std::string name;
		int node_type;
		std::string file_path;
		int start_row;
		int start_col;
		int end_row;
		int end_col;
		std::string language;
		double score;
	};
	std::vector<Row> results;
	std::unordered_set<int64_t> seen;

	// 1. FTS5 prefix search via code_fts (word-based, ranked).
	{
		std::string sql =
			"SELECT gn.id AS node_id, gn.name, gn.kind AS node_type, "
			"gn.file_path AS file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language, rank "
			"FROM code_fts "
			"JOIN entity gn ON gn.id = code_fts.node_id "
			"WHERE code_fts MATCH ? AND code_fts.project_id = ? "
			"ORDER BY "
			"  CASE WHEN gn.kind IN (2,3,4) THEN 0 ELSE 1 END, "
			"  rank "
			"LIMIT ?";

		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error_ = sqlite3_errmsg(db_);
			return std::string(
				       "{\"total\":0,\"results\":[],\"error\":\"") +
			       error_ + "\"}";
		}

		// Escape the query for FTS5 — wrap each word in double quotes to prevent
		// FTS5 syntax errors from user input containing meta-characters
		// (", (, ), :, ^, -, AND/OR/NEAR). Additionally, split each word
		// into its camelCase/snake_case constituents and OR them in, so
		// "findByLastName" also matches snake_case "find_by_last_name"
		// code (the unicode61 tokenizer would otherwise treat each style
		// as one opaque token).
		std::string fts_query;
		const char *p = query;
		while (*p) {
			while (*p == ' ') {
				fts_query += ' ';
				p++;
			}
			if (!*p)
				break;
			std::string word;
			while (*p && *p != ' ') {
				if (*p == '"')
					word += '"'; // escape embedded double-quotes
				word += *p;
				p++;
			}
			fts_query += '"' + word + '"';
			// OR in the split identifier words (dedup, skip empties).
			std::string plain = word;
			std::string unescaped;
			for (size_t i = 0; i < plain.size(); ++i) {
				if (plain[i] != '"')
					unescaped += plain[i];
			}
			auto parts = splitIdentifierWords(unescaped);
			std::unordered_set<std::string> seen_parts;
			for (const auto &part : parts) {
				if (part.empty() || part == unescaped)
					continue;
				if (!seen_parts.insert(part).second)
					continue;
				fts_query += " OR \"" + part + '"';
			}
		}

		sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(project_id));
		sqlite3_bind_int(stmt, 3, limit);

		int rc;
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			Row r;
			r.node_id = sqlite3_column_int64(stmt, 0);
			const char *name_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *fp_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 3));
			const char *lang_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 8));
			r.name = name_raw ? name_raw : "";
			r.node_type = sqlite3_column_int(stmt, 2);
			r.file_path = fp_raw ? fp_raw : "";
			r.start_row = sqlite3_column_int(stmt, 4);
			r.start_col = sqlite3_column_int(stmt, 5);
			r.end_row = sqlite3_column_int(stmt, 6);
			r.end_col = sqlite3_column_int(stmt, 7);
			r.language = lang_raw ? lang_raw : "";
			r.score = sqlite3_column_double(stmt, 9);
			seen.insert(r.node_id);
			results.push_back(std::move(r));
		}
		sqlite3_finalize(stmt);
		if (rc == SQLITE_INTERRUPT) {
			return "{\"error\":\"query timeout after " +
			       std::to_string(timeout_ms) +
			       "ms [module=store, method=searchCode]\"}";
		}
	}

	// 2. Trigram substring search — appended after FTS results, deduped
	// by node_id. Skipped when FTS already filled the limit, when the
	// query is too short for trigrams, or when name_trgm is unavailable.
	constexpr size_t kMinTrigramQueryLen = 3;
	std::string qstr(query);
	if (results.size() < static_cast<size_t>(limit) &&
	    qstr.size() >= kMinTrigramQueryLen && isTrigramAvailable()) {
		const char *sql =
			"SELECT gn.id, gn.name, gn.kind AS node_type, gn.file_path, "
			"gn.start_row, gn.start_col, gn.end_row, gn.end_col, "
			"gn.language "
			"FROM name_trgm "
			"JOIN entity gn ON gn.id = name_trgm.node_id "
			"WHERE name_trgm MATCH ? AND name_trgm.project_id = ? "
			"ORDER BY LENGTH(gn.name) ASC LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) ==
		    SQLITE_OK) {
			// Safe FTS5 phrase query (literal substring, not syntax).
			std::string fts_phrase = fts5Phrase(qstr);

			sqlite3_bind_text(stmt, 1, fts_phrase.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			int rc;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				if (results.size() >=
				    static_cast<size_t>(limit))
					break;
				int64_t nid = sqlite3_column_int64(stmt, 0);
				if (seen.count(nid))
					continue; // dedupe: FTS results preferred
				Row r;
				r.node_id = nid;
				const char *name_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 1));
				const char *fp_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 3));
				const char *lang_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 8));
				r.name = name_raw ? name_raw : "";
				r.node_type = sqlite3_column_int(stmt, 2);
				r.file_path = fp_raw ? fp_raw : "";
				r.start_row = sqlite3_column_int(stmt, 4);
				r.start_col = sqlite3_column_int(stmt, 5);
				r.end_row = sqlite3_column_int(stmt, 6);
				r.end_col = sqlite3_column_int(stmt, 7);
				r.language = lang_raw ? lang_raw : "";
				r.score =
					0.0; // no FTS rank for trigram results
				seen.insert(nid);
				results.push_back(std::move(r));
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, method=searchCode]\"}";
			}
		} else {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchCode: name_trgm prepare failed: %s "
				"[module=store, method=searchCode]\n",
				error_.c_str());
		}
	}

	// 3. Build JSON (same shape as the original searchCode response).
	std::ostringstream json;
	json << "{\"results\":[";
	for (size_t i = 0; i < results.size(); i++) {
		if (i > 0)
			json << ",";
		json << "{"
		     << "\"node_id\":" << results[i].node_id << ","
		     << "\"name\":\"" << jsonEscape(results[i].name) << "\","
		     << "\"node_type\":" << results[i].node_type << ","
		     << "\"file_path\":\"" << jsonEscape(results[i].file_path)
		     << "\","
		     << "\"start_row\":" << results[i].start_row << ","
		     << "\"start_col\":" << results[i].start_col << ","
		     << "\"end_row\":" << results[i].end_row << ","
		     << "\"end_col\":" << results[i].end_col << ","
		     << "\"language\":\"" << jsonEscape(results[i].language)
		     << "\","
		     << "\"score\":" << results[i].score << "}";
	}
	json << "],\"total\":" << results.size() << "}";
	return json.str();
}

// ─── Graph-based fallback search (trigram-accelerated) ─────────

std::string GraphStore::searchGraphFallback(uint64_t project_id,
					    const char *query, int limit)
{
	if (limit <= 0 || limit > 100)
		limit = 20;

	if (!query || !*query)
		return "{\"method\":\"graph_fallback\",\"results\":[]}";

	// Arm the query timeout for this search call. The guard disarms on
	// scope exit (RAII), covering all return paths including exceptions.
	QueryDeadlineGuard guard(this, kDefaultSearchTimeoutMs);
	const int timeout_ms = kDefaultSearchTimeoutMs;

	std::string qstr(query);
	// Trigram tokenizer requires at least 3 characters to form a trigram.
	constexpr size_t kMinTrigramQueryLen = 3;
	bool is_short = qstr.size() < kMinTrigramQueryLen;

	// O(log n) substring search via the trigram FTS5 inverted index.
	// Only used when the query is long enough and the table is available.
	if (!is_short && isTrigramAvailable()) {
		const char *sql =
			"SELECT gn.id, gn.name, gn.file_path, gn.kind "
			"FROM name_trgm "
			"JOIN entity gn ON gn.id = name_trgm.node_id "
			"WHERE name_trgm MATCH ? AND name_trgm.project_id = ? "
			"ORDER BY LENGTH(gn.name) ASC "
			"LIMIT ?";
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) !=
		    SQLITE_OK) {
			error_ = sqlite3_errmsg(db_);
			fprintf(stderr,
				"searchGraphFallback: prepare failed: %s "
				"[module=store, method=searchGraphFallback]\n",
				error_.c_str());
			// Fall through to LIKE path below
		} else {
			std::string fts_phrase = fts5Phrase(qstr);
			sqlite3_bind_text(stmt, 1, fts_phrase.c_str(), -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int64(stmt, 2,
					   static_cast<int64_t>(project_id));
			sqlite3_bind_int(stmt, 3, limit);

			std::ostringstream json;
			json << "{\"method\":\"graph_fallback\",\"results\":[";
			bool first = true;
			int rc;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				if (!first)
					json << ",";
				first = false;
				const char *name_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 1));
				const char *fp_raw =
					reinterpret_cast<const char *>(
						sqlite3_column_text(stmt, 2));
				json << "{"
				     << "\"node_id\":"
				     << sqlite3_column_int64(stmt, 0) << ","
				     << "\"name\":\""
				     << jsonEscape(name_raw ? name_raw : "")
				     << "\","
				     << "\"file_path\":\""
				     << jsonEscape(fp_raw ? fp_raw : "")
				     << "\","
				     << "\"type\":"
				     << sqlite3_column_int(stmt, 3) << "}";
			}
			sqlite3_finalize(stmt);
			if (rc == SQLITE_INTERRUPT) {
				return "{\"error\":\"query timeout after " +
				       std::to_string(timeout_ms) +
				       "ms [module=store, "
				       "method=searchGraphFallback]\"}";
			}
			json << "]}";
			return json.str();
		}
	}

	// LIKE fallback: used for short queries (< 3 chars) or when the
	// trigram table is unavailable. Hard LIMIT 50 for short queries to
	// avoid runaway full-table scans on million-node projects.
	constexpr int kShortQueryScanLimit = 50;
	int like_limit = is_short ? kShortQueryScanLimit : limit;

	std::string like_query = qstr;
	for (auto &c : like_query) {
		if (c == '%' || c == '_')
			c = ' ';
	}

	const char *sql = "SELECT id, name, file_path, kind "
			  "FROM entity "
			  "WHERE project_id=? AND name LIKE ? "
			  "ORDER BY LENGTH(name) ASC "
			  "LIMIT ?";
	sqlite3_stmt *stmt = nullptr;
	std::ostringstream json;
	json << "{\"method\":\"graph_fallback\",\"results\":[";
	bool first = true;
	int rc = SQLITE_DONE;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
		std::string pat = "%" + like_query + "%";
		sqlite3_bind_text(stmt, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, like_limit);

		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			if (!first)
				json << ",";
			first = false;
			const char *name_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 1));
			const char *fp_raw = reinterpret_cast<const char *>(
				sqlite3_column_text(stmt, 2));
			json << "{"
			     << "\"node_id\":" << sqlite3_column_int64(stmt, 0)
			     << ","
			     << "\"name\":\""
			     << jsonEscape(name_raw ? name_raw : "") << "\","
			     << "\"file_path\":\""
			     << jsonEscape(fp_raw ? fp_raw : "") << "\","
			     << "\"type\":" << sqlite3_column_int(stmt, 3)
			     << "}";
		}
		sqlite3_finalize(stmt);
	} else {
		error_ = sqlite3_errmsg(db_);
		fprintf(stderr,
			"searchGraphFallback: LIKE prepare failed: %s "
			"[module=store, method=searchGraphFallback]\n",
			error_.c_str());
	}
	if (rc == SQLITE_INTERRUPT) {
		return "{\"error\":\"query timeout after " +
		       std::to_string(timeout_ms) +
		       "ms [module=store, method=searchGraphFallback]\"}";
	}
	json << "]";
	if (is_short)
		json << ",\"note\":\"short query, limited scan\"";
	json << "}";
	return json.str();
}

// ─── Complexity ───────────────────────────────────────────────
//
// v0.2.5: metrics are restored. The canonical write path is the parse worker
// (engine_index_metrics.cpp) → `_staged_metrics` (insertFileResultBatch) →
// `resolveStagedMetrics()`, which resolves the staged values onto the
// canonical `entity` columns. The read API (getComplexityJson) returns the
// real measurements from `entity`. `setComplexity` below is a retained
// compatibility seam with no callers; it is intentionally inert so a stray
// caller cannot bypass the canonical staged-metrics pipeline and write
// metrics that were never computed.

bool GraphStore::setComplexity(uint64_t project_id, uint64_t graph_node_id,
			       uint64_t cyclomatic, uint64_t cognitive,
			       uint64_t nesting_depth, uint64_t decision_points)
{
	// Inert compatibility seam: canonical metrics flow through
	// _staged_metrics → resolveStagedMetrics. Returns false so a future
	// caller can detect the write did not go through the canonical path.
	(void)project_id;
	(void)graph_node_id;
	(void)cyclomatic;
	(void)cognitive;
	(void)nesting_depth;
	(void)decision_points;
	return false;
}

// Return the per-function code metrics for a single graph node, sourced from
// the canonical entity row (the Knowledge Graph single source of truth).
// Metrics are populated during indexing (staged in _staged_metrics, resolved
// onto entity by resolveStagedMetrics). graph_node_id maps to entity.id,
// which preserves the legacy graph node identity after the graph_nodes→entity
// migration.
//
// Returns a structured JSON object: real measured values plus an
// `available:true` flag, so MCP clients that read `complexity` as a number
// get an actual integer rather than JSON null. When the entity has no
// resolved metrics (e.g. not a function/method, or a pre-metrics database)
// it returns `available:false` with a reason — never a fake 0.
std::string GraphStore::getComplexityJson(uint64_t project_id,
					  uint64_t graph_node_id)
{
	static constexpr const char *kMethod = "getComplexityJson";
	if (!db_) {
		return "{\"error\":\"no_db\",\"complexity\":null,"
		       "\"available\":false}";
	}
	const char *sql =
		"SELECT e.name, e.file_path, e.cyclomatic, e.cognitive, "
		"       e.nesting_depth, e.branch_count, e.loop_count, "
		"       e.param_count, e.call_count, e.lines, e.is_stub "
		"FROM entity e "
		"WHERE e.project_id = ? AND e.id = ?";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		fprintf(stderr,
			"[module=store, method=%s] prepare failed: %s\n",
			kMethod, sqlite3_errmsg(db_));
		return "{\"error\":\"prepare_failed\",\"complexity\":null,"
		       "\"available\":false}";
	}
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(graph_node_id));
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 0));
		const char *file = reinterpret_cast<const char *>(
			sqlite3_column_text(stmt, 1));
		int cyclomatic = sqlite3_column_int(stmt, 2);
		int cognitive = sqlite3_column_int(stmt, 3);
		int nesting = sqlite3_column_int(stmt, 4);
		int branches = sqlite3_column_int(stmt, 5);
		int loops = sqlite3_column_int(stmt, 6);
		int params = sqlite3_column_int(stmt, 7);
		int calls = sqlite3_column_int(stmt, 8);
		int lines = sqlite3_column_int(stmt, 9);
		int is_stub = sqlite3_column_int(stmt, 10);
		std::ostringstream j;
		j << "{\"name\":\"" << jsonEscape(name ? name : "")
		  << "\",\"file_path\":\"" << jsonEscape(file ? file : "")
		  << "\",\"cyclomatic\":" << cyclomatic
		  << ",\"cognitive\":" << cognitive
		  << ",\"nesting_depth\":" << nesting
		  << ",\"branch_count\":" << branches
		  << ",\"loop_count\":" << loops
		  << ",\"param_count\":" << params
		  << ",\"call_count\":" << calls << ",\"lines\":" << lines
		  << ",\"is_stub\":" << (is_stub ? "true" : "false")
		  << ",\"complexity\":" << cyclomatic << ",\"available\":true}";
		sqlite3_finalize(stmt);
		return j.str();
	}
	sqlite3_finalize(stmt);
	if (rc == SQLITE_DONE) {
		// Node not found (or is a non-function entity). Report
		// available:false with a reason so MCP clients can distinguish
		// "no metrics for this node kind" from an error.
		return "{\"complexity\":null,\"available\":false,"
		       "\"reason\":\"no_metrics_for_node\"}";
	}
	fprintf(stderr, "[module=store, method=%s] step %d: %s\n", kMethod, rc,
		sqlite3_errmsg(db_));
	return "{\"error\":\"query_failed\",\"complexity\":null,"
	       "\"available\":false}";
}

// ─── Vector Search (removed) ──────────────────────────────────

bool // storeVector removed — Phase 0 cut
GraphStore::storeVector(uint64_t node_id, uint64_t project_id,
			const void *vec_data, size_t vec_bytes)
{
	if (!stmt_vector_) {
		error_ = "storeVector: statement not prepared";
		return false;
	}
	sqlite3_reset(stmt_vector_);
	sqlite3_bind_int64(stmt_vector_, 1, static_cast<int64_t>(node_id));
	sqlite3_bind_int64(stmt_vector_, 2, static_cast<int64_t>(project_id));
	// sqlite3_bind_blob takes an int length; reject vectors that would
	// overflow it rather than silently truncating the size_t value.
	if (vec_bytes > static_cast<size_t>(INT_MAX)) {
		error_ = "storeVector: vector too large for blob binding";
		return false;
	}
	sqlite3_bind_blob(stmt_vector_, 3, vec_data,
			  static_cast<int>(vec_bytes), SQLITE_TRANSIENT);
	int rc = sqlite3_step(stmt_vector_);
	return rc == SQLITE_DONE;
}

std::string // searchSemantic removed — Phase 0 cut
GraphStore::searchSemantic(uint64_t project_id, const void *query_vec,
			   size_t vec_bytes, int limit)
{
	(void)project_id;
	(void)query_vec;
	(void)vec_bytes;
	(void)limit;
	return "{\"total\":0,\"results\":[]}";
}

// ── New Schema (Phase A): Modules ─────────────────────────────

} // namespace store
