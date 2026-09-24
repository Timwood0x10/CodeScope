#include "lsp_client.h"
#include "lsp_framing.h"
#include "platform_win.h"

#include <fcntl.h>
#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#ifndef _WIN32
#include <sys/select.h>
#include <sys/wait.h>
#endif

// ─── Tunable constants ─────────────────────────────────────────
static constexpr int kSpawnWaitUs =
	100000; // 100 ms grace before checking child
static constexpr size_t kMaxResponseBytes = 1
					    << 20; // 1 MiB cap on LSP response
// Deadline for one write to the server's stdin. The pipe buffer holds ~64KB, so
// a live server drains this well before the deadline; only a server that
// stopped reading its stdin can reach it.
static constexpr int kWriteTimeoutMs = 5000;
// Grace period for the server to exit after the `exit` notification.
static constexpr int kExitGraceMs = 3000;

// ─── JSON-RPC helpers (minimal, no external deps) ─────────────

static std::string buildJsonRpc(const std::string &method,
				const std::string &params, int id)
{
	std::ostringstream body;
	body << "{"
	     << "\"jsonrpc\":\"2.0\","
	     << "\"id\":" << id << ","
	     << "\"method\":\"" << method << "\","
	     << "\"params\":" << params << "}";
	return body.str();
}

static std::string buildNotification(const std::string &method,
				     const std::string &params)
{
	std::ostringstream body;
	body << "{"
	     << "\"jsonrpc\":\"2.0\","
	     << "\"method\":\"" << method << "\","
	     << "\"params\":" << params << "}";
	return body.str();
}

// Wrap body in HTTP-like headers for LSP transport
static std::string wrapLspMessage(const std::string &body)
{
	std::ostringstream msg;
	msg << "Content-Length: " << body.size() << "\r\n\r\n" << body;
	return msg.str();
}

// Message framing (takeNextLspMessage) and response matching (isResponseFor)
// live in lsp_framing.h: they are pure string handling, and the defects they
// fix only show up when a server interleaves notifications with responses, so
// they are tested directly (tests/test_lsp_framing.cpp) instead of through a
// live server.

// ─── LspClient implementation ─────────────────────────────────

LspClient::~LspClient()
{
	if (isRunning())
		stop();
}

bool LspClient::start(const char *command, const char *root_uri)
{
	if (isRunning()) {
		error_ = "already running";
		return false;
	}

	if (!spawnProcess(command))
		return false;

	// Send initialize request
	std::ostringstream init_params;
	init_params << "{"
		    << "\"processId\":null,"
		    << "\"rootUri\":\"" << root_uri << "\","
		    << "\"capabilities\":{}"
		    << "}";

	std::string req =
		buildJsonRpc("initialize", init_params.str(), req_id_);
	if (!sendMessage(req)) {
		stop();
		return false;
	}

	// Read response with timeout; if server dies silently, clean up
	std::string resp = readResponse(req_id_);
	if (resp.empty()) {
		stop();
		return false;
	}
	req_id_++;

	// Send initialized notification (best-effort)
	std::string notif = buildNotification("initialized", "{}");
	sendMessage(notif);

	return true;
}

/// Escape a string for embedding inside a JSON string literal.
/// Handles `"`, `\`, control characters and newlines so a hostile source
/// buffer cannot break the JSON-RPC frame for textDocument/didOpen.
/// @param raw  NUL-terminated C string (may be nullptr → empty).
/// @return Escaped text safe to concatenate into a JSON string value.
static std::string jsonEscapeLsp(const char *raw)
{
	std::string out;
	if (!raw)
		return out;
	out.reserve(std::strlen(raw) + 8);
	for (const char *p = raw; *p; ++p) {
		unsigned char c = static_cast<unsigned char>(*p);
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	return out;
}

bool LspClient::openDocument(const char *file_uri, const char *source_text)
{
	if (!isRunning()) {
		error_ = "server not running";
		return false;
	}

	std::ostringstream params;
	params << "{"
	       << "\"textDocument\":{"
	       << "\"uri\":\"" << jsonEscapeLsp(file_uri) << "\","
	       << "\"languageId\":\"python\","
	       << "\"version\":1,"
	       << "\"text\":\"" << jsonEscapeLsp(source_text) << "\""
	       << "}}";

	std::string notif =
		buildNotification("textDocument/didOpen", params.str());
	return sendMessage(notif);
}

std::string LspClient::queryDefinition(const char *file_uri, int line,
				       int column)
{
	if (!isRunning())
		return "";

	std::ostringstream params;
	params << "{"
	       << "\"textDocument\":{\"uri\":\"" << jsonEscapeLsp(file_uri)
	       << "\"},"
	       << "\"position\":{\"line\":" << line
	       << ",\"character\":" << column << "}"
	       << "}";

	std::string req =
		buildJsonRpc("textDocument/definition", params.str(), req_id_);
	if (!sendMessage(req))
		return "";

	std::string resp = readResponse(req_id_);
	req_id_++;
	return resp;
}

std::string LspClient::queryHover(const char *file_uri, int line, int column)
{
	if (!isRunning())
		return "";

	std::ostringstream params;
	params << "{"
	       << "\"textDocument\":{\"uri\":\"" << jsonEscapeLsp(file_uri)
	       << "\"},"
	       << "\"position\":{\"line\":" << line
	       << ",\"character\":" << column << "}"
	       << "}";

	std::string req =
		buildJsonRpc("textDocument/hover", params.str(), req_id_);
	if (!sendMessage(req))
		return "";

	std::string resp = readResponse(req_id_);
	req_id_++;
	return resp;
}

std::string LspClient::queryDocumentSymbols(const char *file_uri)
{
	if (!isRunning())
		return "";

	std::ostringstream params;
	params << "{"
	       << "\"textDocument\":{\"uri\":\"" << jsonEscapeLsp(file_uri)
	       << "\"}"
	       << "}";

	std::string req = buildJsonRpc("textDocument/documentSymbol",
				       params.str(), req_id_);
	if (!sendMessage(req))
		return "";

	std::string resp = readResponse(req_id_);
	req_id_++;
	return resp;
}

void LspClient::parseDocumentSymbols(
	const std::string &response_body,
	std::unordered_map<std::string, int> &out_symbols)
{
	out_symbols.clear();
	if (response_body.empty())
		return;

	// Parse: [{"name":"...","kind":N,"children":[...]}, ...]
	// We flatten the hierarchy: for each symbol, add name→kind
	std::string search = "\"name\":\"";
	size_t pos = 0;
	while ((pos = response_body.find(search, pos)) != std::string::npos) {
		pos += search.size();
		auto end = response_body.find('"', pos);
		if (end == std::string::npos)
			break;
		std::string name = response_body.substr(pos, end - pos);
		pos = end;

		// Find kind field after this name
		auto kind_pos = response_body.find("\"kind\":", end);
		if (kind_pos == std::string::npos)
			break;
		kind_pos += 7;
		while (kind_pos < response_body.size() &&
		       response_body[kind_pos] == ' ')
			kind_pos++;
		auto kind_end = response_body.find_first_of(",}", kind_pos);
		if (kind_end == std::string::npos)
			break;
		int kind = std::atoi(
			response_body.substr(kind_pos, kind_end - kind_pos)
				.c_str());
		out_symbols[name] = kind;
	}
}

void LspClient::parseSymbolLocations(
	const std::string &response_body,
	std::unordered_map<std::string, std::string> &out_locations)
{
	out_locations.clear();
	if (response_body.empty())
		return;

	// Parse: [{"name":"...","range":{"start":{"line":N,"character":N}},...}, ...]
	std::string search = "\"name\":\"";
	size_t pos = 0;
	while ((pos = response_body.find(search, pos)) != std::string::npos) {
		pos += search.size();
		auto end = response_body.find('"', pos);
		if (end == std::string::npos)
			break;
		std::string name = response_body.substr(pos, end - pos);
		pos = end;

		// Find range
		auto line_pos = response_body.find("\"line\":", end);
		if (line_pos == std::string::npos)
			break;
		line_pos += 7;
		auto line_end = response_body.find_first_of(",}", line_pos);
		if (line_end == std::string::npos)
			break;
		std::string loc =
			"line:" +
			response_body.substr(line_pos, line_end - line_pos);
		out_locations[name] = loc;
	}
}

void LspClient::stop()
{
#ifndef _WIN32
	if (!isRunning())
		return;

	// Send shutdown request
	std::string req = buildJsonRpc("shutdown", "null", req_id_);
	sendMessage(req);
	readResponse(req_id_, 2000);
	req_id_++;

	// Send exit notification
	std::string notif = buildNotification("exit", "null");
	sendMessage(notif);

	// Close pipes
	if (stdin_fd_ >= 0)
		close(stdin_fd_);
	if (stdout_fd_ >= 0)
		close(stdout_fd_);
	stdin_fd_ = -1;
	stdout_fd_ = -1;

	// Reap the child. WNOHANG alone returned immediately even when the server
	// was still running, and because pid_ was cleared straight afterwards
	// nothing ever waited for it again: the process stayed a zombie for the
	// lifetime of the indexer. Wait for it within a grace period, force-kill it
	// if it declines, and report the exit status so a server that failed is not
	// silently treated as a clean shutdown.
	if (pid_ > 0) {
		int status = 0;
		bool reaped = false;
		for (int waited = 0; waited < kExitGraceMs; waited += 50) {
			const pid_t r = waitpid(pid_, &status, WNOHANG);
			if (r == pid_) {
				reaped = true;
				break;
			}
			if (r < 0)
				break; // ECHILD — already reaped elsewhere
			usleep(50 * 1000);
		}
		if (!reaped) {
			kill(pid_, SIGKILL);
			waitpid(pid_, &status, 0); // reap the corpse
			if (error_.empty())
				error_ =
					"LSP server ignored the exit notification; killed";
		} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			if (error_.empty())
				error_ = "LSP server exited with status " +
					 std::to_string(WEXITSTATUS(status));
		}
	}
	pid_ = 0;
#else
	// Windows: send LSP shutdown/exit protocol (same as POSIX),
	// then force kill if the process doesn't exit cleanly.
	if (!isRunning())
		return;

	// Send shutdown request (graceful, same as POSIX path)
	{
		std::string req = buildJsonRpc("shutdown", "null", req_id_);
		sendMessage(req);
		readResponse(req_id_, 2000);
		req_id_++;
	}
	// Send exit notification
	{
		std::string notif = buildNotification("exit", "null");
		sendMessage(notif);
	}

	// Close pipe handles
	if (stdin_fd_ >= 0)
		_close(stdin_fd_);
	if (stdout_fd_ >= 0)
		_close(stdout_fd_);
	stdin_fd_ = -1;
	stdout_fd_ = -1;

	// Wait for graceful exit, then force kill
	if (hProcess_) {
		DWORD wait_rc = WaitForSingleObject(hProcess_, 3000);
		if (wait_rc == WAIT_TIMEOUT)
			TerminateProcess(hProcess_, 1);
		CloseHandle(hProcess_);
		hProcess_ = nullptr;
	}
	pid_ = 0;
#endif
}

// ─── Private helpers ──────────────────────────────────────────

bool LspClient::spawnProcess(const char *command)
{
#ifndef _WIN32
	// Ignore SIGPIPE so a dead LSP server doesn't crash us
	signal(SIGPIPE, SIG_IGN);

	int stdin_pipe[2], stdout_pipe[2];
	if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
		error_ = "pipe() failed";
		return false;
	}

	pid_ = fork();
	if (pid_ < 0) {
		error_ = "fork() failed";
		return false;
	}

	if (pid_ == 0) {
		// Child process: LSP server
		// Redirect stdin to read from parent's pipe
		dup2(stdin_pipe[0], STDIN_FILENO);
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);

		// Redirect stdout to write to parent's pipe
		dup2(stdout_pipe[1], STDOUT_FILENO);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);

		// Redirect stderr to /dev/null
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0)
			dup2(devnull, STDERR_FILENO);

		// Execute LSP server
		execlp(command, command, nullptr);
		// If exec fails, exit
		_exit(1);
	}

	// Parent process
	close(stdin_pipe[0]); // Close read end of stdin pipe
	close(stdout_pipe[1]); // Close write end of stdout pipe

	stdin_fd_ = stdin_pipe[1];
	stdout_fd_ = stdout_pipe[0];

	// Set stdout to non-blocking for reading
	int flags = fcntl(stdout_fd_, F_GETFL, 0);
	fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);

	// Give the child a moment to exec or fail. If it exits immediately,
	// execlp failed (command not found) and we should return false.
	usleep(kSpawnWaitUs);
	int child_status;
	int waited = waitpid(pid_, &child_status, WNOHANG);
	if (waited == pid_) {
		// Child already exited — exec failed
		close(stdin_fd_);
		close(stdout_fd_);
		stdin_fd_ = -1;
		stdout_fd_ = -1;
		pid_ = 0;
		error_ = std::string("failed to start LSP server: ") + command;
		return false;
	}

	return true;
#else
	// Windows: spawn LSP server via CreateProcessW with anonymous pipes.
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = nullptr;
	sa.bInheritHandle = TRUE;

	HANDLE hStdoutRd, hStdoutWr, hStdinRd, hStdinWr;
	if (!CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0) ||
	    !CreatePipe(&hStdinRd, &hStdinWr, &sa, 0)) {
		error_ = "CreatePipe failed for LSP server";
		return false;
	}
	// Ensure the read end of stdout and write end of stdin are not inherited
	SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(hStdinWr, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.hStdInput = hStdinRd;
	si.hStdOutput = hStdoutWr;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	si.dwFlags = STARTF_USESTDHANDLES;

	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));

	// Convert command to wide char for CreateProcessW
	int wlen = MultiByteToWideChar(CP_UTF8, 0, command, -1, nullptr, 0);
	std::wstring wcmd(static_cast<size_t>(wlen), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, command, -1, &wcmd[0], wlen);

	if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, TRUE,
			    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		CloseHandle(hStdoutRd);
		CloseHandle(hStdoutWr);
		CloseHandle(hStdinRd);
		CloseHandle(hStdinWr);
		error_ = std::string("CreateProcessW failed for LSP server: ") +
			 command;
		return false;
	}

	// Close the child-side handles the parent doesn't need
	CloseHandle(hStdoutWr);
	CloseHandle(hStdinRd);

	// Convert Win32 HANDLEs to CRT file descriptors (matching POSIX int fds)
	stdin_fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(hStdinWr),
				    _O_WRONLY | _O_BINARY);
	stdout_fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(hStdoutRd),
				     _O_RDONLY | _O_BINARY);
	pid_ = pi.dwProcessId;
	hProcess_ = pi.hProcess;
	CloseHandle(pi.hThread);

	if (stdin_fd_ < 0 || stdout_fd_ < 0) {
		error_ = "_open_osfhandle failed for LSP pipes";
		stop();
		return false;
	}

	return true;
#endif
}

bool LspClient::sendMessage(const std::string &body)
{
	if (stdin_fd_ < 0) {
		error_ = "stdin pipe not available";
		return false;
	}

	std::string msg = wrapLspMessage(body);
#ifndef _WIN32
	// A blocking write() to a server that stopped reading its stdin never
	// returns, hanging the caller inside what should be a bounded query. Wait
	// for writability with a deadline first.
	if (!waitWritable(kWriteTimeoutMs)) {
		error_ = "timeout writing to LSP server";
		return false;
	}
#endif
#ifndef _WIN32
	ssize_t written = write(stdin_fd_, msg.c_str(), msg.size());
#else
	ssize_t written = _write(stdin_fd_, msg.c_str(), msg.size());
#endif
	if (written < 0 || static_cast<size_t>(written) != msg.size()) {
		// Short write or EINTR: retry the remaining bytes in a loop.
		// A single write() call may return fewer bytes than requested
		// (especially on pipes/sockets under load or after signal
		// delivery). Retrying until completion prevents LSP protocol
		// desynchronization.
		size_t offset = (written > 0) ? static_cast<size_t>(written) :
						0;
		while (offset < msg.size()) {
#ifndef _WIN32
			// Same deadline inside the retry loop: a partial write means the
			// pipe is full, i.e. the server is not draining it.
			if (!waitWritable(kWriteTimeoutMs)) {
				error_ = "timeout writing to LSP server";
				return false;
			}
#endif
#ifndef _WIN32
			ssize_t n = write(stdin_fd_, msg.data() + offset,
					  msg.size() - offset);
#else
			ssize_t n = _write(stdin_fd_, msg.data() + offset,
					   msg.size() - offset);
#endif
			if (n < 0) {
#ifndef _WIN32
				if (errno == EINTR)
					continue;
#endif
				error_ = "write to LSP server failed";
				return false;
			}
			offset += static_cast<size_t>(n);
		}
	}
	return true;
}

#ifndef _WIN32
bool LspClient::waitWritable(int timeout_ms)
{
	struct pollfd pfd;
	pfd.fd = stdin_fd_;
	pfd.events = POLLOUT;
	return poll(&pfd, 1, timeout_ms) > 0;
}
#endif

std::string LspClient::readResponse(int expected_id, int timeout_ms)
{
#ifndef _WIN32
	if (stdout_fd_ < 0)
		return "";

	// Reads accumulate in the persistent read_buffer_: one read can deliver
	// several messages, and one message can span several reads.
	std::string &buffer = read_buffer_;
	auto start_time = time(nullptr);

	while (true) {
		// Check timeout
		if (timeout_ms > 0) {
			struct pollfd pfd;
			pfd.fd = stdout_fd_;
			pfd.events = POLLIN;
			int ret = poll(&pfd, 1, timeout_ms);
			if (ret <= 0) {
				if (ret == 0)
					error_ =
						"timeout waiting for LSP response";
				else
					error_ = "poll() failed";
				return "";
			}
		}

		char buf[4096];
		ssize_t n = 0;
		// Handle EINTR: retry on signal interruption
		do {
			n = read(stdout_fd_, buf, sizeof(buf) - 1);
		} while (n < 0 && errno == EINTR);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			error_ = "read from LSP server failed";
			return "";
		}
		if (n == 0) {
			error_ = "LSP server closed connection";
			return "";
		}
		buf[n] = '\0';
		buffer += buf;

		// Take every message already buffered. Only a response whose id
		// matches this request answers the call: a notification (server
		// logging, diagnostics) or an earlier request's response is skipped
		// and the loop keeps looking, rather than being returned as the
		// answer.
		std::string body;
		for (;;) {
			const LspFraming framed =
				takeNextLspMessage(buffer, body);
			if (framed == LspFraming::Message) {
				if (isResponseFor(body, expected_id))
					return body;
				continue; // notification or another request's response
			}
			if (framed == LspFraming::Skipped)
				continue; // unusable frame dropped, try the next
			break; // NeedMore — read more bytes
		}

		// Prevent infinite loop on malformed data
		if (buffer.size() > kMaxResponseBytes) {
			error_ = "response too large";
			return "";
		}

		if (timeout_ms > 0 &&
		    (time(nullptr) - start_time) > (timeout_ms / 1000)) {
			error_ = "timeout";
			return "";
		}
	}
#else
	// Windows: read response via PeekNamedPipe + ReadFile (no poll()).
	if (stdout_fd_ < 0)
		return "";

	// Same persistent buffer as the POSIX path (see read_buffer_).
	std::string &buffer = read_buffer_;
	auto start_time = GetTickCount64();

	while (true) {
		// Check timeout via PeekNamedPipe (non-blocking)
		DWORD bytes_avail = 0;
		if (!PeekNamedPipe((HANDLE)_get_osfhandle(stdout_fd_), nullptr,
				   0, nullptr, &bytes_avail, nullptr)) {
			// Pipe closed / error
			break;
		}

		if (bytes_avail > 0) {
			char buf[4096];
			DWORD bytes_read = 0;
			if (!ReadFile((HANDLE)_get_osfhandle(stdout_fd_), buf,
				      sizeof(buf) - 1, &bytes_read, nullptr)) {
				error_ = "ReadFile from LSP server failed";
				return "";
			}
			if (bytes_read == 0) {
				error_ = "LSP server closed connection";
				return "";
			}
			buf[bytes_read] = '\0';
			buffer += buf;

			// Same message framing and id matching as the POSIX path: a
			// notification is not the answer to this request.
			std::string body;
			for (;;) {
				const LspFraming framed =
					takeNextLspMessage(buffer, body);
				if (framed == LspFraming::Message) {
					if (isResponseFor(body, expected_id))
						return body;
					continue;
				}
				if (framed == LspFraming::Skipped)
					continue;
				break;
			}
		}

		// Timeout check
		if (timeout_ms > 0 && (GetTickCount64() - start_time) >
					      static_cast<DWORD>(timeout_ms)) {
			error_ = "timeout waiting for LSP response";
			return "";
		}

		// Prevent infinite loop on malformed data
		if (buffer.size() > kMaxResponseBytes) {
			error_ = "response too large";
			return "";
		}

		Sleep(10); // 10ms poll interval
	}

	error_ = "LSP server closed connection";
	return "";
#endif
}

// ─── Static helpers ────────────────────────────────────────────

bool LspClient::isAvailable(const char *command)
{
	if (!command || !*command)
		return false;
#ifndef _WIN32
	// Safe check: use access() for absolute paths, PATH search without shell
	if (command[0] == '/') {
		return access(command, X_OK) == 0;
	}
	// Search PATH manually (no shell invocation)
	const char *path_env = getenv("PATH");
	if (!path_env)
		return false;
	std::string path(path_env);
	size_t start = 0, end;
	while ((end = path.find(':', start)) != std::string::npos) {
		std::string dir = path.substr(start, end - start);
		std::string full = dir + "/" + command;
		if (access(full.c_str(), X_OK) == 0)
			return true;
		start = end + 1;
	}
	// Last entry (no trailing colon)
	std::string full = path.substr(start) + "/" + command;
	return access(full.c_str(), X_OK) == 0;
#else
	// Windows: search PATH manually with `\\` separator
	if (command[0] == '/' || command[0] == '\\' ||
	    (command[0] != '\0' && command[1] == ':')) {
		return _access(command, 0) == 0; // absolute or drive path
	}
	const char *path_env = getenv("PATH");
	if (!path_env)
		return false;
	std::string path(path_env);
	size_t start = 0, end;
	while ((end = path.find(';', start)) != std::string::npos) {
		std::string dir = path.substr(start, end - start);
		if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
			dir += '\\';
		std::string full = dir + command;
		if (_access(full.c_str(), 0) == 0)
			return true;
		// Try with .exe extension
		if (_access((full + ".exe").c_str(), 0) == 0)
			return true;
		start = end + 1;
	}
	// Last entry (no trailing semicolon)
	{
		std::string dir = path.substr(start);
		if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
			dir += '\\';
		std::string full = dir + command;
		if (_access(full.c_str(), 0) == 0)
			return true;
		if (_access((full + ".exe").c_str(), 0) == 0)
			return true;
	}
	return false;
#endif
}

std::string LspClient::extractTargetUri(const std::string &response_body)
{
	if (response_body.empty())
		return "";

	// Handle empty/error responses
	if (response_body.find("\"result\":null") != std::string::npos)
		return "";

	// Try to find "uri" in a Location object: {"uri":"file:///...","range":{...}}
	// This handles both single Location and Location array
	std::string uri_marker = "\"uri\":\"";
	auto pos = response_body.find(uri_marker);
	if (pos == std::string::npos)
		return "";

	pos += uri_marker.size();
	auto end = response_body.find('"', pos);
	if (end == std::string::npos)
		return "";

	std::string uri = response_body.substr(pos, end - pos);

	// Strip file:// prefix for cleaner representation
	if (uri.compare(0, 7, "file://") == 0) {
		uri = uri.substr(7);
	}

	return uri;
}

std::string LspClient::extractHoverContent(const std::string &response_body)
{
	if (response_body.empty())
		return "";

	// Hover response structure: { "contents": { "kind": "markdown", "value":
	// "..." } } or { "contents": "type info" }
	auto val_pos = response_body.find("\"value\":\"");
	if (val_pos != std::string::npos) {
		val_pos += 9; // skip "value":"
		auto end = response_body.find('"', val_pos);
		if (end != std::string::npos) {
			return response_body.substr(val_pos, end - val_pos);
		}
	}

	// Fallback: try to extract the "contents" string directly
	auto cont_pos = response_body.find("\"contents\":\"");
	if (cont_pos != std::string::npos) {
		cont_pos += 12;
		auto end = response_body.find('"', cont_pos);
		if (end != std::string::npos) {
			return response_body.substr(cont_pos, end - cont_pos);
		}
	}

	return "";
}
