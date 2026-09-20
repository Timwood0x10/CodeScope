#ifndef LSP_FRAMING_H
#define LSP_FRAMING_H

// LSP message framing and response matching.
//
// Split out of lsp_client.cpp so the parsing rules can be tested directly: they
// are pure string handling, and the bugs they fix are invisible from the
// public API (they only show up when a real server interleaves notifications
// with responses).

#include <cstdlib>
#include <string>

/// Result of one framing attempt.
enum class LspFraming {
	Message, // out_body holds a complete message; its bytes were consumed
	NeedMore, // the buffer holds no complete message yet
	Skipped, // an unusable frame was discarded; try the next one
};

// Try to take one complete LSP-framed message out of `buffer`.
//
// LSP frames each message as `Content-Length: N\r\n\r\n<body>`; a stream read
// can deliver several messages, part of one, or a whole message plus the start
// of the next.
//
// The third result (Skipped) is not decoration: when junk framing is discarded
// the caller must come back for the message behind it rather than wait for more
// bytes. Reporting that as NeedMore stalls a caller whose buffer already holds
// a complete answer.
inline LspFraming takeNextLspMessage(std::string &buffer, std::string &out_body)
{
	auto header_end = buffer.find("\r\n\r\n");
	if (header_end == std::string::npos)
		return LspFraming::NeedMore;
	auto cl_pos = buffer.find("Content-Length:");
	if (cl_pos == std::string::npos || cl_pos > header_end)
		return LspFraming::NeedMore;
	auto val_start = cl_pos + 15; // after "Content-Length:"
	while (val_start < buffer.size() && buffer[val_start] == ' ')
		val_start++;
	auto val_end = buffer.find_first_of("\r\n", val_start);
	if (val_end == std::string::npos)
		return LspFraming::NeedMore;
	int content_length = std::atoi(
		buffer.substr(val_start, val_end - val_start).c_str());
	if (content_length <= 0) {
		// A header without a usable length can never be framed. Drop it so
		// the caller does not spin on the same bytes forever.
		buffer.erase(0, header_end + 4);
		return LspFraming::Skipped;
	}
	const size_t body_start = header_end + 4;
	if (buffer.size() < body_start + static_cast<size_t>(content_length))
		return LspFraming::NeedMore;
	out_body =
		buffer.substr(body_start, static_cast<size_t>(content_length));
	buffer.erase(0, body_start + static_cast<size_t>(content_length));
	return LspFraming::Message;
}

// Wrap a JSON-RPC body in LSP framing.
inline std::string wrapLspFramed(const std::string &body)
{
	return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
	       body;
}

// True when `body` is a JSON-RPC RESPONSE carrying `expected_id`.
//
// A notification carries no id, and a response to an earlier request carries a
// different one: neither answers the call in progress. Returning whichever
// message arrived first handed the caller a `window/logMessage` notification
// where it expected a definition result.
inline bool isResponseFor(const std::string &body, int expected_id)
{
	auto pos = body.find("\"id\"");
	if (pos == std::string::npos)
		return false;
	pos += 4;
	while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':'))
		pos++;
	if (pos >= body.size() || body[pos] < '0' || body[pos] > '9')
		return false;
	return std::atoi(body.c_str() + pos) == expected_id;
}

#endif // LSP_FRAMING_H
