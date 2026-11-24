#!/usr/bin/env bash
# Executa o port. Os defaults de ambiente (executor de pushbuffer, janela,
# vblank, VMA estendida, KEVENTs, APU) sao aplicados pelo proprio binario
# (src/hooks/recomp_manual.c); qualquer variavel RECOMP_* definida aqui vence.
#
#   ./run.sh                      # teclado como controle da porta 0
#   DW4_SHOT=build/shots/s ./run.sh   # grava BMPs do que esta na tela
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"
exec "$ROOT/build/dw4_recomp" "$ROOT/game_data" "$@"
