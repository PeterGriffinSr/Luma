#!/bin/bash
# test_partial.sh
# Tests that the LSP doesn't hang on incomplete/partial input.
# Run: ./test_partial.sh 2>debug.log
# If it hangs (no output after a few seconds), you've reproduced the bug.

LUMA_BIN="${1:-./luma}"
TIMEOUT=5  # seconds to wait for each response

send_message() {
  local msg="$1"
  local len
  len=$(printf '%s' "$msg" | wc -c)
  printf "Content-Length: %d\r\n\r\n%s" "$len" "$msg"
}

run_test() {
  local description="$1"
  local content="$2"

  echo "Testing: $description" >&2

  # Run with a timeout — if it hangs, timeout kills it
  output=$(timeout $TIMEOUT bash -c "
    {
      $(declare -f send_message)
      send_message '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}'
      send_message '{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}'
      send_message '{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///test.lx\",\"languageId\":\"luma\",\"version\":1,\"text\":\"@module \\\"test\\\"\n\"}}}'
      send_message '{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///test.lx\",\"version\":2},\"contentChanges\":[{\"text\":\"$content\"}]}}'
      send_message '{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"shutdown\",\"params\":null}'
      send_message '{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}'
    } | $LUMA_BIN -lsp
  " 2>/dev/null)

  local exit_code=$?
  if [ $exit_code -eq 124 ]; then
    echo "  HANG DETECTED (timed out after ${TIMEOUT}s) — this is the bug!" >&2
    echo "  HANG: $description"
  elif [ $exit_code -ne 0 ]; then
    echo "  CRASH (exit $exit_code)" >&2
    echo "  CRASH: $description"
  else
    echo "  OK" >&2
  fi
}

# Each of these simulates typing a partial keyword
run_test "just 'c'"          "c"
run_test "partial 'con'"     "con"
run_test "partial 'cons'"    "cons"
run_test "partial 'const '"  "const "
run_test "unterminated string" "const x = \""
run_test "unclosed brace"    "const foo -> fn () int {"
run_test "valid module"      "@module \\\"test\\\"\nconst main -> fn () int {\n\treturn 0;\n};"