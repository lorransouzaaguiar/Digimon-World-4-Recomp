#!/usr/bin/env bash
# Monta o pacote de Windows (x86-64) a partir do Linux.
#
#   DW4_URL=<url do projeto> scripts/package_win.sh vX.Y.Z
#
# Gera em dist/:
#   DigimonWorld4-Recomp-<versao>-windows-x86_64.zip      o pacote
#   DigimonWorld4-Recomp-<versao>-windows-x86_64.debug.exe  o mesmo executavel COM
#       simbolos; fica so aqui, para traduzir os enderecos de um relatorio de crash:
#       x86_64-w64-mingw32-addr2line -f -e <.debug.exe> <0x140000000 + deslocamento>
#
# Conteudo do .zip: dw4_recomp.exe (sem simbolos), SDL2.dll, libepoxy-0.dll, run.bat, README.txt,
# LICENSE.txt, licenses/, game_data/ (vazia). Ainda sem launcher.
#
# Falha de proposito se achar arquivo do jogo, caminho da maquina de build no
# executavel ou uma DLL que nao deveria ser importada (DirectX, runtime do MinGW).
set -euo pipefail

VERSION="${1:?uso: scripts/package_win.sh vX.Y.Z}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
URL="${DW4_URL:-https://github.com/lorransouzaaguiar/Digimon-World-4-Recomp}"
TRIPLET="${MINGW_TRIPLET:-x86_64-w64-mingw32}"
BUILD="$ROOT/build-win-release"
DIST="$ROOT/dist"
NAME="DigimonWorld4-Recomp-$VERSION-windows-x86_64"
PKG="$DIST/$NAME"
SDL2_ROOT="$(ls -d "$ROOT"/tools/win-deps/SDL2-*/ 2>/dev/null | sort -V | tail -1 || true)"

die() { echo "package_win: ERRO: $*" >&2; exit 1; }

# Build de release: sem DW4_PROJECT_ROOT (o jogo e os saves sao procurados ao lado
# do executavel) e sem caminhos da maquina de build nas informacoes de depuracao.
WIN_BUILD_DIR="$BUILD" DW4_CMAKE_ARGS="-DDW4_RELEASE=ON" scripts/build_win.sh > "$ROOT/build-win-release.out" 2>&1 ||
    { tail -20 "$ROOT/build-win-release.out" >&2; die "o build falhou (build-win-release.out)"; }
rm -f "$ROOT/build-win-release.out"
SDL2_ROOT="$(ls -d "$ROOT"/tools/win-deps/SDL2-*/ | sort -V | tail -1)"

rm -rf "$PKG" "$PKG.zip"
mkdir -p "$PKG/game_data" "$PKG/licenses"
cp "$BUILD/dw4_recomp.exe" "$DIST/$NAME.debug.exe"
"$TRIPLET-strip" -o "$PKG/dw4_recomp.exe" "$BUILD/dw4_recomp.exe"
cp "$SDL2_ROOT/$TRIPLET/bin/SDL2.dll" "$BUILD/libepoxy-0.dll" "$PKG/"
EPOXY_ROOT="$(ls -d "$ROOT"/tools/win-deps/epoxy-[0-9]*/ | sort -V | tail -1)"

for f in run.bat README.txt; do
    sed -e "s|@VERSION@|$VERSION|g" -e "s|@URL@|$URL|g" "packaging/windows/$f" > "$PKG/$f"
done
printf 'Copie aqui os arquivos do SEU disco de Digimon World 4 (Xbox, NTSC-U):\r\ndefault.xbe, area.afs, ndmw.afs, ndmw.igb\r\n\r\nCopy the files of YOUR OWN Digimon World 4 disc (Xbox, NTSC-U) here.\r\n' \
    > "$PKG/game_data/LEIA-ME.txt"

cp LICENSE "$PKG/LICENSE.txt"
cp xboxrecomp/LICENSE "$PKG/licenses/xboxrecomp-MIT.txt"
cp xboxrecomp/NOTICE "$PKG/licenses/xboxrecomp-NOTICE.txt"
cp xboxrecomp/LICENSES/LGPL-2.1.txt "$PKG/licenses/LGPL-2.1.txt"
cp "$SDL2_ROOT/LICENSE.txt" "$PKG/licenses/SDL2-zlib.txt"
cp "$EPOXY_ROOT/COPYING" "$PKG/licenses/libepoxy-MIT.txt"
cat > "$PKG/licenses/MinGW-w64.txt" <<'EOT'
dw4_recomp.exe is built with GCC for MinGW-w64 and statically links:
  - the MinGW-w64 runtime and winpthreads (https://www.mingw-w64.org/),
    under their permissive licenses (ZPL 2.1 / MIT / BSD-style);
  - libgcc, under the GPL-3.0 with the GCC Runtime Library Exception, which
    allows distribution of the compiled program under any terms.
SDL2.dll is the unmodified official build from https://github.com/libsdl-org/SDL
(zlib license, see SDL2-zlib.txt); it may be replaced by another SDL 2 build.
libepoxy-0.dll is libepoxy built unmodified from https://github.com/anholt/libepoxy
(MIT license, see libepoxy-MIT.txt).
EOT

# ── Barreiras ────────────────────────────────────────────────────────────────
BAD="$(find "$PKG" -type f \( -iname '*.xbe' -o -iname '*.afs' -o -iname '*.igb' -o -iname '*.iso' \
        -o -iname '*.img' -o -iname '*.bmp' \) | head -5)"
[ -z "$BAD" ] || die "arquivo do jogo ou de save dentro do pacote: $BAD"

LEAK="$(grep -a -c -F -e "$ROOT" -e "$HOME" "$PKG/dw4_recomp.exe" || true)"
[ "$LEAK" = "0" ] || die "o executavel contem caminhos da maquina de build ($LEAK ocorrencias)"

DLLS="$("$TRIPLET-objdump" -p "$PKG/dw4_recomp.exe" | sed -n 's/.*DLL Name: //p' | sort -u)"
for d in $DLLS; do
    case "$(echo "$d" | tr 'A-Z' 'a-z')" in
        kernel32.dll|shell32.dll|winmm.dll|user32.dll|gdi32.dll|opengl32.dll|sdl2.dll|vulkan-1.dll|\
        libepoxy-0.dll|api-ms-win-crt-*.dll) ;;
        *) die "o executavel importa $d (DirectX ou runtime que nao vai no pacote?)" ;;
    esac
done

# zip pelo Python (o programa "zip" nao vem em toda distribuicao).
(cd "$DIST" && python3 - "$NAME" <<'PY'
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, dirs, files in os.walk(name):
        dirs.sort()
        for d in dirs:
            if not os.listdir(os.path.join(base, d)):
                z.write(os.path.join(base, d))
        for f in sorted(files):
            z.write(os.path.join(base, f))
PY
)
(cd "$DIST" && sha256sum "$NAME.zip" > "SHA256SUMS-$VERSION-windows.txt" &&
    cat "SHA256SUMS-$VERSION-windows.txt")
echo "DLLs importadas: $(echo $DLLS)"
echo "pacote: $PKG.zip ($(du -h "$PKG.zip" | cut -f1))"
echo "simbolos (nao publicar): $DIST/$NAME.debug.exe"
