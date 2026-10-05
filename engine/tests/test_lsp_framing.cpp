// Regression tests for LSP message framing and response matching.
//
// The defects these cover are invisible through the public API: they need a
// server that interleaves notifications with responses, or that splits a
// message across reads. The parsing rules are pure string handling, so they are
// driven directly.

#include "../src/lsp/lsp_framing.h"

#include "test_check.h"
#include <cstdio>
#include <string>

int main()
{
	// ── Test 1: a complete message is taken out of the buffer ───────
	{
		const std::string response =
			"{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}";
		std::string buffer = wrapLspFramed(response);
		std::string body;
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(body == response);
		CHECK(buffer.empty());
		CHECK(isResponseFor(body, 7));
		// A response to a different request is not this call's answer.
		CHECK(!isResponseFor(body, 8));
		printf("  [PASS] framing: complete message extracted and matched\n");
	}

	// ── Test 2: a message split across reads ────────────────────────
	{
		const std::string response =
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"uri\":\"a\"}}";
		const std::string framed = wrapLspFramed(response);
		// Header only, then part of the body, then the rest.
		std::string buffer = framed.substr(0, 20);
		std::string body;
		CHECK(takeNextLspMessage(buffer, body) ==
		       LspFraming::NeedMore);
		buffer += framed.substr(20, 10);
		CHECK(takeNextLspMessage(buffer, body) ==
		       LspFraming::NeedMore);
		buffer += framed.substr(30);
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(body == response);
		CHECK(buffer.empty());
		printf("  [PASS] framing: message reassembled across reads\n");
	}

	// ── Test 3: notification before the response (the reported bug) ──
	// The old readResponse() returned the FIRST complete message, so this
	// window/logMessage became the "definition result" the caller acted on,
	// and the buffered bytes that followed it were discarded.
	{
		const std::string notification =
			"{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\","
			"\"params\":{\"type\":3,\"message\":\"indexing\"}}";
		const std::string response =
			"{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"ok\":true}}";
		std::string buffer =
			wrapLspFramed(notification) + wrapLspFramed(response);
		std::string body;

		// The notification is framed correctly but is not an answer.
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(body == notification);
		CHECK(!isResponseFor(body, 11));

		// The response that arrived in the same read is still there.
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(body == response);
		CHECK(isResponseFor(body, 11));
		CHECK(buffer.empty());
		printf("  [PASS] framing: notification skipped, response kept\n");
	}

	// ── Test 4: a notification alone never matches ──────────────────
	{
		std::string buffer = wrapLspFramed(
			"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"publishDiagnostics\",\"params\":{\"uri\":\"f\"}}");
		std::string body;
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(!isResponseFor(body, 1));
		printf("  [PASS] framing: notification is never a response\n");
	}

	// ── Test 5: an unusable Content-Length cannot stall the caller ──
	// Reporting a discarded frame as "need more bytes" would leave a caller
	// waiting for data that is not coming, while a complete answer sits in the
	// buffer behind the junk.
	{
		std::string buffer = "Content-Length: 0\r\n\r\n";
		const std::string tail = wrapLspFramed(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}");
		buffer += tail;
		std::string body;
		// The zero-length header is reported as consumed, not as "need more"...
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Skipped);
		CHECK(buffer == tail);
		// ...so the message behind it is readable immediately.
		CHECK(takeNextLspMessage(buffer, body) == LspFraming::Message);
		CHECK(isResponseFor(body, 2));
		CHECK(buffer.empty());

		std::string only_bad = "Content-Length: 0\r\n\r\n";
		CHECK(takeNextLspMessage(only_bad, body) ==
		       LspFraming::Skipped);
		CHECK(only_bad.empty());
		printf("  [PASS] framing: unusable header dropped, next message read\n");
	}

	// ── Test 6: a non-numeric id is not a response id ───────────────
	{
		CHECK(!isResponseFor("{\"jsonrpc\":\"2.0\",\"id\":\"abc\"}",
				      1));
		CHECK(!isResponseFor("{\"jsonrpc\":\"2.0\",\"params\":{}}",
				      1));
		CHECK(isResponseFor("{\"jsonrpc\":\"2.0\",\"id\": 42 }", 42));
		printf("  [PASS] framing: non-numeric / missing ids rejected\n");
	}

	printf("=== test_lsp_framing PASSED ===\n");
	return checkFailures() ? 1 : 0;
}
