#!/usr/bin/env bash
# Prepara um clone novo: submodulo do toolkit, patches locais e ambiente Python.
#
#   scripts/setup.sh
#
# Depois: copie o disco do jogo para game_data/, rode scripts/regen.sh e compile
# (ver README.md). Pode ser rodado de novo sem problema.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

git submodule update --init xboxrecomp

PATCH="$ROOT/patches/xboxrecomp-dw4.patch"
if git -C xboxrecomp apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "toolkit: patches ja aplicados"
elif git -C xboxrecomp apply --check "$PATCH" 2>/dev/null; then
    git -C xboxrecomp apply "$PATCH"
    echo "toolkit: patches aplicados (patches/xboxrecomp-dw4.patch)"
else
    echo "toolkit: o patch nao aplica nesta revisao do xboxrecomp." >&2
    echo "         O submodulo deve estar no commit registrado no repositorio." >&2
    exit 1
fi

if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
fi
.venv/bin/pip install -q -r requirements.txt -r launcher/requirements.txt
echo "python: .venv pronto (capstone, ninja)"

mkdir -p game_data
if ! find game_data -maxdepth 1 -iname default.xbe | grep -q .; then
    echo
    echo "Falta o jogo: extraia o disco de Digimon World 4 (Xbox, NTSC-U) para game_data/"
    echo "(default.xbe, area.afs, ndmw.afs, ndmw.igb). Depois: scripts/regen.sh"
fi
