#!/usr/bin/env bash
# Regenera src/recompiled/ a partir de game_data/Default.xbe.
#
#   scripts/regen.sh            # disasm + func_id + abi + recomp
#   scripts/regen.sh --lift     # so o recomp (reaproveita analysis/)
#
# Sementes de funcoes (alvos so alcancados por ponteiro) ficam em config/:
#   config/seed_functions.json      varredura estatica (scripts/scan_code_pointers.py)
#   config/seed_observed.json       alvos vistos em execucao (tools.seed_from_log)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
[ -x "$PY" ] || PY=python3
XBE="$(find "$ROOT/game_data" -maxdepth 1 -iname default.xbe | head -1)"
A="$ROOT/analysis"
GEN="$ROOT/src/recompiled"

cd "$ROOT/xboxrecomp"

if [ "${1:-}" != "--lift" ]; then
    "$PY" -m tools.xbe_parser "$XBE" --json "$A/Default_analysis.json" > "$A/xbe_parser.log"

    SEEDS=()
    for f in "$ROOT"/config/seed_*.json; do
        [ -f "$f" ] && SEEDS+=(--seed-functions "$f")
    done
    "$PY" -m tools.disasm "$XBE" --analysis-json "$A/Default_analysis.json" -v --force -o "$A/disasm" "${SEEDS[@]}" > "$A/disasm.log" 2>&1
    "$PY" -m tools.func_id "$XBE" --functions "$A/disasm/functions.json" \
        --strings "$A/disasm/strings.json" --xrefs "$A/disasm/xrefs.json" \
        -o "$A/func_id" -v > "$A/func_id.log" 2>&1
    "$PY" -m tools.abi_analysis "$XBE" --disasm-dir "$A/disasm" \
        --func-id-dir "$A/func_id" --output-dir "$A/abi" -v > "$A/abi.log" 2>&1
fi

# Chunks antigos podem sobrar se a contagem de funcoes diminuir.
rm -f "$GEN"/recomp_[0-9]*.c "$GEN"/recomp_dispatch.c "$GEN"/recomp_stubs_unresolved.c "$GEN"/recomp_funcs.h

EXTRA=()
[ -f "$ROOT/config/icall_sites.json" ] && EXTRA+=(--icall-sites "$ROOT/config/icall_sites.json")

"$PY" -m tools.recomp "$XBE" --game-name "Digimon World 4" --all --split 250 \
    --gen-dir "$GEN" --disasm-dir "$A/disasm" --func-id-dir "$A/func_id" \
    --abi-dir "$A/abi" -o "$A/recomp" "${EXTRA[@]}" > "$A/recomp.log" 2>&1

grep -E "functions \(|unresolved call|unimplemented" "$A/recomp.log" || true
tr '\r' '\n' < "$A/disasm.log" | grep -A12 "Functions by section" || true
