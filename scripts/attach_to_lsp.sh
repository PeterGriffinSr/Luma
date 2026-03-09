#!/bin/bash
# attach_to_lsp.sh
# Run this in a terminal right after VS Code hangs (while it's timing out).
# It finds the luma LSP process and dumps its stack trace.

PID=$(pgrep -f "luma.*-lsp" | head -1)

if [ -z "$PID" ]; then
  echo "No luma -lsp process found. Is VS Code still running?"
  exit 1
fi

echo "Found luma LSP process: PID $PID"
echo ""
echo "=== Stack trace ==="
# Try gdb first
if command -v gdb &>/dev/null; then
  gdb -batch \
      -ex "attach $PID" \
      -ex "thread apply all bt" \
      -ex "detach" \
      -ex "quit" \
      2>/dev/null
elif command -v lldb &>/dev/null; then
  lldb -p "$PID" \
       --batch \
       -o "thread backtrace all" \
       -o "detach" \
       -o "quit" \
       2>/dev/null
else
  echo "Neither gdb nor lldb found. Showing /proc stack:"
  cat /proc/$PID/wchan
  echo ""
  # On Linux, show the syscall the process is stuck in
  cat /proc/$PID/status | grep -E "Name|State|VmRSS"
  echo ""
  echo "Stack (requires root or ptrace permissions):"
  cat /proc/$PID/stack 2>/dev/null || echo "Cannot read /proc/$PID/stack (need root)"
fi