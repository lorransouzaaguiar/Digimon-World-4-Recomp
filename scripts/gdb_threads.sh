#!/usr/bin/env bash
# Roda o jogo sob gdb, interrompe apos N segundos e despeja o backtrace de
# todas as threads (os frames sub_XXXXXXXX sao a cadeia de chamadas do guest).
#   scripts/gdb_threads.sh [segundos] > build/gdb_threads.txt
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-12}"
( sleep "$SECS"; pkill -INT -x dw4_recomp ) &
RECOMP_KEYBOARD=1 gdb -q -batch \
    -ex 'set pagination off' -ex 'set confirm off' \
    -ex 'handle SIGSEGV nostop noprint pass' -ex 'handle SIGBUS nostop noprint pass' \
    -ex 'handle SIGUSR1 nostop noprint pass' -ex 'handle SIGUSR2 nostop noprint pass' \
    -ex run -ex 'thread apply all bt 30' -ex kill \
    --args "$ROOT/build/dw4_recomp" 2>&1
