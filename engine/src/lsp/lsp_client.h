#ifndef LSP_CLIENT_H
#define LSP_CLIENT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

/**
 * Minimal LSP (Language Server Protocol) client.
 *
 * Communicates with an LSP server via JSON-RPC over stdin/stdout.
 * Used to resolve fully-qualified symbol names and disambiguate
 * call targets during code indexing.
 *
 * Usage:
 *   LspClient client;
 *   if (client.start("pylsp", "/tmp")) {       // start LSP server
 *       client.openDocument("/tmp/test.py",     // notify server of file
 *                           "def foo(): pass");
 *       auto def = client.queryDefinition(      // get definition location
 *           "/tmp/test.py", 0, 5);
 *       client.stop();                           // shutdown server
 *   }
 *
 * Thread-safety: NOT thread-safe. Create one per thread if needed.
 */

class LspClient {
    public:
	LspClient() = default;
	~LspClient();

	// Non-copyable
	LspClient(const LspClient &) = delete;
	LspClient &operator=(const LspClient &) = delete;

	/**
     * Start an LSP server process.
     *
     * @param command  Server command, e.g. "pylsp" or "clangd".
     * @param root_uri Project root directory (file:// URI).
     * @return true if the server started and initialized successfully.
     */
	bool start(const char *command, const char *root_uri);

	/**
     * Notify the server that a document was opened.
     * Must be called before queryDefinition for a given file.
     */
	bool openDocument(const char *file_uri, const char *source_text);

	/**
     * Query textDocument/definition for a symbol at the given position.
     *
     * @param file_uri  The file URI.
     * @param line      0-based line.
     * @param column    0-based column.
     * @return JSON-RPC result string, or empty on failure.
     */
	std::string queryDefinition(const char *file_uri, int line, int column);

	/**
     * Query textDocument/hover for type information at a position.
     *
     * @return Hover contents string, or empty if unavailable.
     */
	std::string queryHover(const char *file_uri, int line, int column);

	/**
     * Query textDocument/documentSymbol for all symbols in a file.
     *
     * Parses the response and populates a name→range map for fast
     * local symbol resolution without per-symbol LSP queries.
     *
     * @param file_uri  The file URI.
     * @return Raw JSON-RPC response body for parsing.
     */
	std::string queryDocumentSymbols(const char *file_uri);

	/**
     * Parse a documentSymbol response and extract name→kind mapping.
     *
     * @param response_body  Raw JSON body from queryDocumentSymbols.
     * @param out_symbols    Output map: name → NodeKind (0=Function, 12=Class,
     * etc).
     */
	static void
	parseDocumentSymbols(const std::string &response_body,
			     std::unordered_map<std::string, int> &out_symbols);

	/**
     * Parse a documentSymbol response and extract name→location mapping.
     *
     * @param response_body  Raw JSON body from queryDocumentSymbols.
     * @param out_locations  Output map: name → "file://path:line:col"
     */
	static void parseSymbolLocations(
		const std::string &response_body,
		std::unordered_map<std::string, std::string> &out_locations);

	/**
     * Shutdown and stop the LSP server.
     */
	void stop();

	/** Returns true if the server is running. */
	bool isRunning() const
	{
		return pid_ > 0;
	}

	/** Returns the last error message. */
	const std::string &error() const
	{
		return error_;
	}

	/**
     * Check if an LSP server command is available on the system PATH.
     * Used before attempting to start a server.
     */
	static bool isAvailable(const char *command);

	/**
     * Extract the target URI from a textDocument/definition response.
     * The response is a JSON-RPC result body containing either a single
     * Location or a LocationLink/array.
     */
	std::string extractTargetUri(const std::string &response_body);

	/**
     * Extract readable content from a textDocument/hover response.
     */
	std::string extractHoverContent(const std::string &response_body);

    private:
	int pid_ = 0; // LSP server process PID
	int stdin_fd_ = -1; // Write end of pipe to server stdin
	int stdout_fd_ = -1; // Read end of pipe from server stdout
	int req_id_ = 1; // JSON-RPC request ID counter
	std::string error_;

	// Spawn the server process with pipes for stdin/stdout.
	bool spawnProcess(const char *command);

	// Send a JSON-RPC message to the server.
	bool sendMessage(const std::string &json_body);

	// Read a JSON-RPC response from the server (blocks until received).
	// Returns the body of the response, or empty on timeout/failure.
	std::string readResponse(int expected_id, int timeout_ms = 5000);
};

#endif // LSP_CLIENT_H
