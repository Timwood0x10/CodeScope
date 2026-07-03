#include "lsp_client.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

// ─── JSON-RPC helpers (minimal, no external deps) ─────────────

static std::string buildJsonRpc(const std::string &method,
                                const std::string &params, int id) {
  std::ostringstream body;
  body << "{"
       << "\"jsonrpc\":\"2.0\","
       << "\"id\":" << id << ","
       << "\"method\":\"" << method << "\","
       << "\"params\":" << params << "}";
  return body.str();
}

static std::string buildNotification(const std::string &method,
                                     const std::string &params) {
  std::ostringstream body;
  body << "{"
       << "\"jsonrpc\":\"2.0\","
       << "\"method\":\"" << method << "\","
       << "\"params\":" << params << "}";
  return body.str();
}

// Wrap body in HTTP-like headers for LSP transport
static std::string wrapLspMessage(const std::string &body) {
  std::ostringstream msg;
  msg << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  return msg.str();
}

// ─── LspClient implementation ─────────────────────────────────

LspClient::~LspClient() {
  if (isRunning())
    stop();
}

bool LspClient::start(const char *command, const char *root_uri) {
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

  std::string req = buildJsonRpc("initialize", init_params.str(), req_id_);
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

bool LspClient::openDocument(const char *file_uri, const char *source_text) {
  if (!isRunning()) {
    error_ = "server not running";
    return false;
  }

  std::ostringstream params;
  params << "{"
         << "\"textDocument\":{"
         << "\"uri\":\"" << file_uri << "\","
         << "\"languageId\":\"python\","
         << "\"version\":1,"
         << "\"text\":\"" << source_text << "\""
         << "}}";

  std::string notif = buildNotification("textDocument/didOpen", params.str());
  return sendMessage(notif);
}

std::string LspClient::queryDefinition(const char *file_uri, int line,
                                       int column) {
  if (!isRunning())
    return "";

  std::ostringstream params;
  params << "{"
         << "\"textDocument\":{\"uri\":\"" << file_uri << "\"},"
         << "\"position\":{\"line\":" << line << ",\"character\":" << column
         << "}"
         << "}";

  std::string req =
      buildJsonRpc("textDocument/definition", params.str(), req_id_);
  if (!sendMessage(req))
    return "";

  std::string resp = readResponse(req_id_);
  req_id_++;
  return resp;
}

std::string LspClient::queryHover(const char *file_uri, int line, int column) {
  if (!isRunning())
    return "";

  std::ostringstream params;
  params << "{"
         << "\"textDocument\":{\"uri\":\"" << file_uri << "\"},"
         << "\"position\":{\"line\":" << line << ",\"character\":" << column
         << "}"
         << "}";

  std::string req = buildJsonRpc("textDocument/hover", params.str(), req_id_);
  if (!sendMessage(req))
    return "";

  std::string resp = readResponse(req_id_);
  req_id_++;
  return resp;
}

std::string LspClient::queryDocumentSymbols(const char *file_uri) {
  if (!isRunning())
    return "";

  std::ostringstream params;
  params << "{"
         << "\"textDocument\":{\"uri\":\"" << file_uri << "\"}"
         << "}";

  std::string req =
      buildJsonRpc("textDocument/documentSymbol", params.str(), req_id_);
  if (!sendMessage(req))
    return "";

  std::string resp = readResponse(req_id_);
  req_id_++;
  return resp;
}

void LspClient::parseDocumentSymbols(
    const std::string &response_body,
    std::unordered_map<std::string, int> &out_symbols) {
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
    while (kind_pos < response_body.size() && response_body[kind_pos] == ' ')
      kind_pos++;
    auto kind_end = response_body.find_first_of(",}", kind_pos);
    if (kind_end == std::string::npos)
      break;
    int kind =
        std::atoi(response_body.substr(kind_pos, kind_end - kind_pos).c_str());
    out_symbols[name] = kind;
  }
}

void LspClient::parseSymbolLocations(
    const std::string &response_body,
    std::unordered_map<std::string, std::string> &out_locations) {
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
        "line:" + response_body.substr(line_pos, line_end - line_pos);
    out_locations[name] = loc;
  }
}

void LspClient::stop() {
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

  // Wait for process to exit
  if (pid_ > 0) {
    int status;
    waitpid(pid_, &status, WNOHANG);
  }
  pid_ = 0;
}

// ─── Private helpers ──────────────────────────────────────────

bool LspClient::spawnProcess(const char *command) {
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
  close(stdin_pipe[0]);  // Close read end of stdin pipe
  close(stdout_pipe[1]); // Close write end of stdout pipe

  stdin_fd_ = stdin_pipe[1];
  stdout_fd_ = stdout_pipe[0];

  // Set stdout to non-blocking for reading
  int flags = fcntl(stdout_fd_, F_GETFL, 0);
  fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);

  // Give the child a moment to exec or fail. If it exits immediately,
  // execlp failed (command not found) and we should return false.
  usleep(100000); // 100ms
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
}

bool LspClient::sendMessage(const std::string &body) {
  if (stdin_fd_ < 0) {
    error_ = "stdin pipe not available";
    return false;
  }

  std::string msg = wrapLspMessage(body);
  ssize_t written = write(stdin_fd_, msg.c_str(), msg.size());
  if (written < 0 || static_cast<size_t>(written) != msg.size()) {
    error_ = "write to LSP server failed";
    return false;
  }
  return true;
}

std::string LspClient::readResponse(int expected_id, int timeout_ms) {
  if (stdout_fd_ < 0)
    return "";

  // Read until we find Content-Length header + body
  std::string buffer;
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
          error_ = "timeout waiting for LSP response";
        else
          error_ = "poll() failed";
        return "";
      }
    }

    char buf[4096];
    ssize_t n = read(stdout_fd_, buf, sizeof(buf) - 1);
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

    // Check if we have a complete message (Content-Length header + body)
    auto header_end = buffer.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      // Parse Content-Length
      auto cl_pos = buffer.find("Content-Length:");
      if (cl_pos == std::string::npos)
        continue;

      auto val_start = cl_pos + 15; // after "Content-Length:"
      while (val_start < buffer.size() && buffer[val_start] == ' ')
        val_start++;
      auto val_end = buffer.find_first_of("\r\n", val_start);
      if (val_end == std::string::npos)
        continue;

      int content_length =
          std::atoi(buffer.substr(val_start, val_end - val_start).c_str());
      if (content_length <= 0)
        continue;

      size_t body_start = header_end + 4; // after \r\n\r\n
      if (buffer.size() >= body_start + static_cast<size_t>(content_length)) {
        std::string body = buffer.substr(body_start, content_length);

        // In a real implementation, we'd match the id.
        // For our minimal case, just return the body.
        (void)expected_id;
        return body;
      }
    }

    // Prevent infinite loop on malformed data
    if (buffer.size() > 1024 * 1024) {
      error_ = "response too large";
      return "";
    }

    if (timeout_ms > 0 && (time(nullptr) - start_time) > (timeout_ms / 1000)) {
      error_ = "timeout";
      return "";
    }
  }
}

std::string LspClient::extractStringField(const std::string &json,
                                          const std::string &key) {
  std::string search = "\"" + key + "\":\"";
  auto pos = json.find(search);
  if (pos == std::string::npos)
    return "";
  pos += search.size();
  auto end = json.find('"', pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
}

// ─── Static helpers ────────────────────────────────────────────

bool LspClient::isAvailable(const char *command) {
  if (!command || !*command)
    return false;
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
}

std::string LspClient::extractTargetUri(const std::string &response_body) {
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

std::string LspClient::extractHoverContent(const std::string &response_body) {
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
