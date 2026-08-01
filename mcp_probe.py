#!/usr/bin/env python3
"""MCP stdio test client for CodeScope — calls MCP tools against a real project.

Usage: CODESCOPE_DB_PATH=... python3 mcp_probe.py <tool_name> '<json_args>'
Prints the raw JSON-RPC result content (tools/call content[0].text or error).
"""
import json
import os
import subprocess
import sys

SERVER = os.environ.get(
    "CODESCOPE_SERVER", "/Users/scc/code/cppCode/CodeScope/target/release/codescope"
)


def main():
    tool = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}

    env = dict(os.environ)
    env.pop("CODESCOPE_SERVER", None)

    proc = subprocess.Popen(
        [SERVER],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
    )

    def send(msg):
        proc.stdin.write(json.dumps(msg) + "\n")
        proc.stdin.flush()

    def recv():
        line = proc.stdout.readline()
        return json.loads(line) if line.strip() else None

    # 1. initialize
    send(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "mcp_probe", "version": "0.1"},
            },
        }
    )
    init_resp = recv()

    # 2. call tool
    send(
        {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        }
    )
    resp = recv()

    # give the server a moment to flush any async error to stderr
    try:
        proc.stdin.close()
    except Exception:
        pass
    proc.wait(timeout=60)
    err = proc.stderr.read()[-2000:]

    if resp is None:
        print("NO RESPONSE")
        if err:
            print("STDERR:", err)
        sys.exit(1)

    if "error" in resp:
        print("RPC_ERROR:", json.dumps(resp["error"], ensure_ascii=False))
        sys.exit(2)

    result = resp.get("result", {})
    content = result.get("content", [])
    for c in content:
        if isinstance(c, dict) and "text" in c:
            print(c["text"])
    if result.get("isError"):
        print("TOOL_ERROR_FLAG=true")
    if err:
        print("STDERR:", err)


if __name__ == "__main__":
    main()
