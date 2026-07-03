# CodeScope Skills

CodeScope is an MCP-based code understanding service. It parses source code, builds a code graph (call graph + symbol reference graph), and exposes graph queries via MCP tools.

## Available commands

```bash
# Build everything
make build

# Run all tests
make test

# Run engine tests only
make test-engine

# Build grammars (after npm install)
make build-grammars

# Build Rust server
make build-server
```

## MCP Tool Reference

### Core tools (always available)

| Tool | Query | Response |
|------|-------|----------|
| `find_definition` | symbol name | file + line/col range |
| `find_references` | symbol name | all referencing locations |
| `get_callers` | function name | functions that call it |
| `get_callees` | function name | functions it calls |
| `get_neighbors` | node ID | incoming + outgoing neighbors |
| `find_shortest_path` | source + target IDs | node path between them |
| `get_subgraph` | center node ID | subgraph nodes + edges |
| `locate_code` | symbol name or node ID | location in source file |
| `index_project` | project path | index all supported files |
| `index_file` | file path | index a single file |
| `get_graph_stats` | — | node/edge/file counts |

### Phase 1 tools (P0 capability)

| Tool | Query | Response |
|------|-------|----------|
| `search_code` | text query (prefix supported) | matching nodes + relevance score |
| `get_complexity` | graph node ID | cyclomatic + nesting depth |
| `graph_query` | `MATCH (type)-[edge]->(type)` | matching triples |

### Phase 2 tools (differentiation)

| Tool | Query | Response |
|------|-------|----------|
| `detect_changes` | JSON array of modified files | modified/callers/callees/total_impacted |
| `get_communities` | — | community clusters + inter-community edges |

## Usage patterns

### Quick start
```bash
# Start the MCP server
cargo run --bin codescope

# In another terminal, send MCP messages:
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | nc localhost 8080
```

### Index a project
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "index_project",
    "arguments": {
      "project_path": "/path/to/your/project"
    }
  }
}
```

### Query patterns
```json
// Find all function calls
{"name": "graph_query", "arguments": {"query": "MATCH (Function)-[Calls]->(Function)"}}

// Search for a symbol
{"name": "search_code", "arguments": {"query": "parseFile", "limit": 10}}

// Get function complexity
{"name": "get_complexity", "arguments": {"node_id": 1}}

// Detect change impact
{"name": "detect_changes", "arguments": {"modified_files": "[\"/path/to/changed.py\"]"}}
```

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ASTGRAPH_DB_PATH` | `/tmp/astgraph.db` | SQLite database path |
| `GRAMMARS_DIR` | `grammars/` | tree-sitter grammar .so dir |
| `CODESCOPE_LSP` | (unset) | LSP server command for type enhancement (e.g. `pylsp`) |
