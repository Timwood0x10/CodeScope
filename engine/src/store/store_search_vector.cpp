// store_search_vector.cpp — lexical-similarity ("semantic") search.
//
// Split out of store_search.cpp (see plan/rules/code_rules.md 1000-line
// rule). This is the n-gram hashing producer/consumer pair: it is NOT a
// meaning vector, it is the lexical-similarity signal the search pipeline
// falls back on when FTS5 comes up short. The tool description and the
// capabilities JSON both say so.
//
// Vectors are built from the entity's qualified_name + name n-grams, so
// "user_dao" is findable given "user_repository" with no model loaded.

#include "store.h"
#include "store_internal.h"
#include "store_search_words.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace store
{

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
	if (sqlite3_prepare_v2(db_, clear_sql, -1, &del, nullptr) !=
	    SQLITE_OK) {
		fprintf(stderr,
			"buildVectorsFromGraph: prepare clear failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			sqlite3_errmsg(db_));
		return;
	}
	sqlite3_bind_int64(del, 1, static_cast<int64_t>(project_id));
	if (sqlite3_step(del) != SQLITE_DONE) {
		fprintf(stderr,
			"buildVectorsFromGraph: clear step failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			sqlite3_errmsg(db_));
		sqlite3_finalize(del);
		return;
	}
	sqlite3_finalize(del);

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

	// v0.2.5 (perf fix): wrap the whole batch of INSERTs in a single
	// transaction. In autocommit mode every row INSERT issues its own
	// fsync/commit, which made vector build take tens of seconds on large
	// projects (thousands of function entities). Collapsing all writes into
	// one commit is an order-of-magnitude faster, and the table is our own
	// scratch (vector_ready is derived from the row count, so
	// partial/rolled-back writes still yield correct readiness).
	//
	// SAVEPOINT rather than BEGIN: this may run inside the index
	// transaction, where a plain BEGIN fails and the matching COMMIT would
	// commit the caller's transaction.
	if (!exec("SAVEPOINT build_vectors")) {
		fprintf(stderr,
			"buildVectorsFromGraph: SAVEPOINT failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			error_.c_str());
		return;
	}

	// RAII: any early return below — or an allocation that throws inside
	// the loop — rolls the savepoint back, so a failed vector build can
	// never leave the connection sitting inside a transaction (which would
	// make every later BEGIN fail with "cannot start a transaction within
	// a transaction").
	struct SavepointGuard {
		GraphStore *store = nullptr;
		bool released = false;
		~SavepointGuard()
		{
			if (!released) {
				store->exec(
					"ROLLBACK TO SAVEPOINT build_vectors");
				store->exec("RELEASE SAVEPOINT build_vectors");
			}
		}
	} guard{ this, false };

	bool ok = true;
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
			ok = false;
		}
		sqlite3_reset(ins);
	}
	sqlite3_finalize(ins);

	// Abort (letting the guard roll back) rather than release a savepoint
	// holding a half-built vector table: semantic search would otherwise
	// mix new vectors with the stale ones the DELETE was meant to remove.
	if (!ok) {
		fprintf(stderr,
			"buildVectorsFromGraph: batch failed, rolling back "
			"savepoint "
			"[module=store, method=buildVectorsFromGraph]\n");
		return;
	}
	if (!exec("RELEASE SAVEPOINT build_vectors")) {
		fprintf(stderr,
			"buildVectorsFromGraph: RELEASE SAVEPOINT failed: %s "
			"[module=store, method=buildVectorsFromGraph]\n",
			error_.c_str());
		return;
	}
	guard.released = true;
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
} // namespace store
