#!/usr/bin/env bash
# Teste de plugar/desplugar controle sem jogo, sem janela e sem hardware
# (tests/pad_hotplug.c). Usa a camada de entrada do toolkit como o jogo usa.
#   scripts/test_pad.sh [ciclos]      (padrao 300; ~4 s)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RT="$ROOT/xboxrecomp/src"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
"${CC:-cc}" -std=gnu11 -Wall -O1 -g -o "$OUT/pad_hotplug" \
    "$ROOT/tests/pad_hotplug.c" "$RT/input/xinput_device.c" "$RT/input/keyboard.c" "$RT/input/input_map.c" \
    -I"$RT/input" -I"$RT" -I"$RT/kernel" $(pkg-config --cflags --libs sdl2) -lpthread -lm
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$OUT/pad_hotplug" "${1:-300}"
