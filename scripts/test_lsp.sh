#!/bin/bash
# test_lsp.sh — sends a full LSP session with correct Content-Length values
# Usage: ./test_lsp.sh 2>lsp_debug.log | cat

set -euo pipefail

LSP_BIN="${1:-./luma}"

send_message() {
  local msg="$1"
  # Use printf %s to avoid interpreting escape sequences in the message itself
  local len
  len=$(printf '%s' "$msg" | wc -c)
  printf "Content-Length: %d\r\n\r\n%s" "$len" "$msg"
}

{
  send_message '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'

  send_message '{"jsonrpc":"2.0","method":"initialized","params":{}}'

  send_message '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///test.lx","languageId":"luma","version":1,"text":"let x: int = 1;"}}}'

  send_message '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///test.lx","version":2},"contentChanges":[{"text":"let x: int = 2;\nlet y: int = x + 1;"}]}}'

  send_message '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test.lx"},"position":{"line":0,"character":4}}}'

  send_message '{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///test.lx"},"position":{"line":1,"character":0}}}'

  send_message '{"jsonrpc":"2.0","id":99,"method":"shutdown","params":null}'

  send_message '{"jsonrpc":"2.0","method":"exit","params":null}'

} | "$LSP_BIN" -lsp