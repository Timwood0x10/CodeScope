// store_identity.cpp — content-derived identity for findings and source state.
//
// Contract: plan/0.3.x/development_plan.md §17.4-§17.6.
//
// Two identities live here:
//   * FindingKey(rule_id, anchor) — the cross-version identity of an issue.
//     It must NOT contain the revision, the rule version or a row id, or the
//     same issue gets a new identity every time anything around it changes.
//   * GraphStore::worktreeDigest(project_id) — the identity of the source
//     state a run was taken against. A revision alone cannot express a dirty
//     worktree, so a re-run over modified files would look identical.
//
// SHA-256 is implemented locally (~150 lines) rather than pulled in as a
// dependency: the engine links nothing but SQLite today.

#include "store.h"
#include "store_identity.h"

#include <cstdint>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace store
{
namespace
{

constexpr uint32_t kRoundConstants[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
	0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
	0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
	0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
	0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
	0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
	0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
	0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
	0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
	0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t Rotr(uint32_t value, uint32_t bits)
{
	return (value >> bits) | (value << (32 - bits));
}

/// Streaming SHA-256 state. The digest is built incrementally because the
/// worktree digest is computed row by row from SQLite.
struct Sha256 {
	uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
	uint64_t total_bytes = 0;
	uint8_t block[64] = {};
	size_t block_len = 0;

	void Compress(const uint8_t *chunk)
	{
		uint32_t w[64];
		for (int i = 0; i < 16; ++i) {
			w[i] = (uint32_t(chunk[i * 4]) << 24) |
			       (uint32_t(chunk[i * 4 + 1]) << 16) |
			       (uint32_t(chunk[i * 4 + 2]) << 8) |
			       uint32_t(chunk[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i) {
			uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^
				      (w[i - 15] >> 3);
			uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^
				      (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
		uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; ++i) {
			uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + s1 + ch + kRoundConstants[i] + w[i];
			uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = s0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}

	void Update(std::string_view data)
	{
		total_bytes += data.size();
		for (char raw : data) {
			block[block_len++] = static_cast<uint8_t>(raw);
			if (block_len == sizeof(block)) {
				Compress(block);
				block_len = 0;
			}
		}
	}

	std::string HexDigest()
	{
		// Padding: 0x80, zeros, then the bit length as a big-endian u64.
		const uint64_t bit_len = total_bytes * 8;
		uint8_t pad[72] = { 0x80 };
		size_t pad_len = (block_len < 56) ? (56 - block_len) :
						    (120 - block_len);
		Update(std::string_view(reinterpret_cast<char *>(pad),
					pad_len));

		uint8_t len_be[8];
		for (int i = 0; i < 8; ++i)
			len_be[i] =
				static_cast<uint8_t>(bit_len >> (56 - 8 * i));
		Update(std::string_view(reinterpret_cast<char *>(len_be), 8));

		static const char *kHex = "0123456789abcdef";
		std::string out;
		out.reserve(64);
		for (uint32_t word : h) {
			for (int shift = 28; shift >= 0; shift -= 4)
				out.push_back(kHex[(word >> shift) & 0xf]);
		}
		return out;
	}
};

} // namespace

std::string Sha256Hex(std::string_view data)
{
	Sha256 state;
	state.Update(data);
	return state.HexDigest();
}

std::string FindingKey(const std::string &rule_id, const std::string &anchor)
{
	return Sha256Hex(rule_id + "\n" + anchor);
}

std::string GraphStore::worktreeDigest(uint64_t project_id)
{
	if (!db_) {
		error_ = "worktreeDigest: no connection";
		return {};
	}

	// Sorted by path, so the digest does not depend on insertion order
	// (worker merges and re-indexes insert in different orders).
	const char *sql = "SELECT path, content_hash FROM files "
			  "WHERE project_id = ? ORDER BY path";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
		error_ = std::string("worktreeDigest: ") + sqlite3_errmsg(db_);
		return {};
	}
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(project_id));

	Sha256 state;
	int rc = SQLITE_ROW;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		const char *path = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 0));
		const char *hash = reinterpret_cast<const char *>(
			sqlite3_column_text(st, 1));
		state.Update(path ? path : "");
		state.Update("\n");
		state.Update(hash ? hash : "");
		state.Update("\n");
	}
	sqlite3_finalize(st);
	// A step error must not be reported as a digest of the rows seen so far:
	// that value would look valid and silently describe a partial worktree.
	if (rc != SQLITE_DONE) {
		error_ = std::string("worktreeDigest: step failed: ") +
			 sqlite3_errmsg(db_);
		return {};
	}
	// A project with no files yields the digest of the empty string, which is
	// still a valid (and stable) identity.
	return state.HexDigest();
}

} // namespace store
