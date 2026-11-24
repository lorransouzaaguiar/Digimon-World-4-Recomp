#!/usr/bin/env bash
# Abre o launcher do Digimon World 4 - Recomp.  ./launcher.sh --play inicia o jogo direto.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for PY in "$HERE/launcher/python/bin/python3" "$HERE/.venv/bin/python" python3; do
    if "$PY" -c "import PySide6.QtQuick" 2>/dev/null; then
        exec "$PY" "$HERE/launcher/dw4_launcher.py" "$@"
    fi
done
echo "O launcher precisa do PySide6 (Qt para Python)." >&2
echo "  Arch/CachyOS:  sudo pacman -S pyside6" >&2
echo "  Qualquer um:   python3 -m venv .venv && .venv/bin/pip install -r launcher/requirements.txt" >&2
echo "Sem o launcher, o jogo abre com ./run.sh" >&2
exit 1
