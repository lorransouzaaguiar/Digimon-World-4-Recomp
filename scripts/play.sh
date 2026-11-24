#!/usr/bin/env bash
# Roda o jogo por N segundos com um roteiro de entrada e gera uma folha de
# contato das capturas.   scripts/play.sh <segundos> "<roteiro>" [intervalo]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
SECS="${1:-60}"; SCRIPT="${2:-}"; EVERY="${3:-3}"
mkdir -p build/shots; rm -f build/shots/p*.bmp build/shots/p*.png
DW4_SHOT=build/shots/p DW4_SHOT_EVERY="$EVERY" DW4_INPUT_SCRIPT="$SCRIPT" \
    RECOMP_KEYBOARD=1 timeout -s KILL "$SECS" ./build/dw4_recomp > build/run.log 2>&1
for f in build/shots/p*.bmp; do ffmpeg -loglevel error -y -i "$f" "${f%.bmp}.png"; done
N=$(ls build/shots/p*.png | wc -l)
ffmpeg -loglevel error -y -pattern_type glob -i 'build/shots/p*.png' \
    -vf "scale=320:240,tile=5x$(( (N + 4) / 5 ))" build/shots/sheet.png
echo "capturas: $N"
grep -E 'INPUT\]|CRASH|ICALL\] Failed|UNIMPL|decode fail|STUB\]|MMIO\]' build/run.log | sort | uniq -c | sort -rn | head -30
