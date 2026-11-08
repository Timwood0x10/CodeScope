// test_identity.cpp — content-derived identity (development_plan §17.6).
//
// Guards three properties that a refactor could silently break:
//
//   1. SHA-256 is correct. The implementation is local, so it needs published
//      vectors rather than a round-trip against itself.
//   2. A finding key depends on the rule and the anchor ONLY. Changing the
//      README (or any source file) must not produce a new identity, or every
//      re-run would look like a brand new set of issues.
//   3. The worktree digest covers the whole project, is order-independent and
//      changes when any file's content changes.

#include "../src/store/store.h"
#include "../src/store/store_identity.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static char kDbPath[160];

static void initDbPath()
{
	std::snprintf(kDbPath, sizeof(kDbPath),
		      "/tmp/codescope_test_identity_%d.db", int(getpid()));
}

static void check(bool cond, const char *msg)
{
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

static void checkEq(const std::string &got, const std::string &want,
		    const char *msg)
{
	if (got != want) {
		std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", msg,
			     got.c_str(), want.c_str());
		std::exit(1);
	}
}

static void removeDbFiles()
{
	unlink(kDbPath);
	std::string wal = std::string(kDbPath) + "-wal";
	std::string shm = std::string(kDbPath) + "-shm";
	unlink(wal.c_str());
	unlink(shm.c_str());
}

/// Published SHA-256 test vectors (FIPS 180-4 / NIST examples).
static void testSha256Vectors()
{
	checkEq(store::Sha256Hex(""),
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		"sha256 of the empty string");
	checkEq(store::Sha256Hex("abc"),
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"sha256 of \"abc\"");
	// 56 bytes: the padding has to start a second block.
	checkEq(store::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomn"
				 "opnopq"),
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
		"sha256 of the 56-byte NIST vector");
	std::printf("  [PASS] SHA-256 vectors\n");
}

/// The digest must not depend on how the caller streams the data.
static void testSha256Streaming()
{
	const std::string data(1000, 'a');
	checkEq(store::Sha256Hex(data),
		"41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
		"sha256 of 1000 x 'a'");
	std::printf("  [PASS] SHA-256 streaming input\n");
}

static void testFindingKey()
{
	const std::string key =
		store::FindingKey("size.file_too_large", "symbol:Big::Parse");
	checkEq(key,
		store::FindingKey("size.file_too_large", "symbol:Big::Parse"),
		"finding key must be deterministic");
	check(key.size() == 64, "finding key must be 64 hex characters");
	check(key != store::FindingKey("size.other", "symbol:Big::Parse"),
	      "different rule must produce a different key");
	check(key != store::FindingKey("size.file_too_large", "symbol:Big::Read"),
	      "different anchor must produce a different key");
	// The separator must prevent boundary collisions.
	check(store::FindingKey("ab", "c") != store::FindingKey("a", "bc"),
	      "rule/anchor boundary must not collide");
	// Anchors are opaque strings here; the tie-breaker anchors (file#nth)
	// simply produce different keys, which is all this layer guarantees.
	check(store::FindingKey("r", "file:a.cpp#0") !=
		      store::FindingKey("r", "file:a.cpp#1"),
	      "tie-breaker anchors must differ");
	std::printf("  [PASS] finding key\n");
}

int main()
{
	initDbPath();
	removeDbFiles();

	testSha256Vectors();
	testSha256Streaming();
	testFindingKey();

	store::GraphStore store;
	check(store.open(kDbPath), "open database");
	uint64_t project_id = store.createProject("/test/identity", "identity");
	check(project_id > 0, "createProject");

	// An empty project still has a stable identity (the empty-string digest).
	checkEq(store.worktreeDigest(project_id), store::Sha256Hex(""),
		"empty project digest must be the empty-string digest");

	store.upsertFile(project_id, "src/a.cpp", "cpp", "hash-a1");
	store.upsertFile(project_id, "README.md", "markdown", "hash-r1");
	const std::string digest_before = store.worktreeDigest(project_id);
	checkEq(store.worktreeDigest(project_id), digest_before,
		"worktree digest must be deterministic");
	check(digest_before.size() == 64,
	      "worktree digest must be 64 hex characters");

	// Changing ANY file's content changes the source state.
	store.upsertFile(project_id, "README.md", "markdown", "hash-r2");
	const std::string digest_after = store.worktreeDigest(project_id);
	check(digest_after != digest_before,
	      "a content change must change the worktree digest");

	// Insertion order must not matter: the digest is sorted by path.
	uint64_t project_b = store.createProject("/test/identity-b", "identity-b");
	store.upsertFile(project_b, "README.md", "markdown", "hash-r2");
	store.upsertFile(project_b, "src/a.cpp", "cpp", "hash-a1");
	checkEq(store.worktreeDigest(project_b), digest_after,
		"insertion order must not affect the digest");

	// A finding's identity is independent of the source state: this is what
	// makes "only the README changed" a no-op for existing findings.
	const std::string key_before =
		store::FindingKey("size.file_too_large", "symbol:Big");
	store.upsertFile(project_id, "README.md", "markdown", "hash-r3");
	checkEq(store::FindingKey("size.file_too_large", "symbol:Big"),
		key_before,
		"finding key must survive a source change");
	check(store.worktreeDigest(project_id) != digest_after,
	      "the source state itself must have moved on");

	store.close();
	removeDbFiles();
	std::printf("\nAll identity tests passed.\n");
	return 0;
}
