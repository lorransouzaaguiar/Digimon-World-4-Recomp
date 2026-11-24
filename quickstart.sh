#!/usr/bin/env bash
# Atalho de teste: pula a abertura, passa pela tela titulo, entra com o
# Controller 1, cria um Digimon (Dorumon, nome "a"), entra no lobby e passa o
# dialogo inicial -- tudo por roteiro (tests/scenarios/quickstart.txt). Depois
# o controle/teclado e seu; a janela fica aberta ate voce fechar.
#
#   ./quickstart.sh                       # ~75 s ate o controle ser seu
#   ./quickstart.sh > build/run.log 2>&1
#
# Nao toque no controle enquanto o roteiro roda. Ele e ancorado em frames do
# jogo, nao em tempo, entao nao depende da velocidade da maquina.
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD:-1}"
export DW4_INPUT_SCRIPT="@$ROOT/tests/scenarios/quickstart.txt"
exec "$ROOT/build/dw4_recomp" "$ROOT/game_data" "$@"
