#!/usr/bin/env bash
# Apaga de dist/ os pacotes de OUTRAS versoes (regra do projeto: ao criar uma
# versao nova, as antigas saem de dist/). Chamado por scripts/package_release.sh
# e scripts/package_win.sh; pode ser rodado a mao.
#
#   scripts/dist_prune.sh vX.Y.Z      # fica so o que e da vX.Y.Z
#
# So mexe no que estes scripts geram (DigimonWorld4-Recomp-v* e SHA256SUMS-v*).
# Vai junto o ...windows-x86_64.debug.exe das versoes antigas (os simbolos para
# traduzir um relatorio de crash delas): guarde-o fora de dist/ se for precisar.
set -euo pipefail
VERSION="${1:?uso: scripts/dist_prune.sh vX.Y.Z}"
DIST="$(cd "$(dirname "$0")/.." && pwd)/dist"
[ -d "$DIST" ] || exit 0
for p in "$DIST"/DigimonWorld4-Recomp-v* "$DIST"/SHA256SUMS-v*; do
    [ -e "$p" ] || continue
    case "$(basename "$p")" in
        "DigimonWorld4-Recomp-$VERSION-"*|"SHA256SUMS-$VERSION.txt"|"SHA256SUMS-$VERSION-"*) ;;
        *) echo "dist: removendo $(basename "$p") (outra versao)"; rm -rf "$p" ;;
    esac
done
