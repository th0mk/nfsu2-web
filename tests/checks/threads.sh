#!/bin/bash
# Suite check for "threads" (web/tools/run-suite.mjs, named only): NFSU2_THREAD_TEST
# made 33 guest threads at once before the game started (runtime/win32_ext.c):
# the 33rd got no stack and was refused loudly, and no thread's 1 MB stack
# reached into the heap at 0x08000000 (runtime/memmap.h).
LOG="$1"
grep -qE '^\[thread\] REFUSED: CreateThread\(entry 0x[0-9a-f]+\) gets NULL \(ERROR_NOT_ENOUGH_MEMORY\): all 32 thread stacks 0x06000000-0x08000000 are in use' "$LOG" \
  || { echo "the 33rd thread was not refused"; exit 1; }
top=$(grep -oE '^\[thread\] start .* stack=0x[0-9a-f]{8}' "$LOG" | sed -E 's/.*stack=//' | sort -u | tail -1)
[ -n "$top" ] || { echo "no thread started"; exit 1; }
[ $(( top + 0x100000 )) -le $(( 0x08000000 )) ] || { echo "the thread stack at $top reaches into the heap"; exit 1; }
echo "the 33rd thread was refused; the highest stack is $top-$(printf '0x%08x' $(( top + 0x100000 )))"
