#!/usr/bin/env bash
# Monta o pacote de Windows (x86-64) a partir do Linux.
#
#   DW4_URL=<url do projeto> scripts/package_win.sh vX.Y.Z
#
# Gera em dist/:
#   DigimonWorld4-Recomp-<versao>-windows-x86_64.zip        o pacote
#   DigimonWorld4-Recomp-<versao>-windows-x86_64.debug.exe  o executavel do jogo COM
#       simbolos; fica so aqui, para traduzir os enderecos de um relatorio de crash:
#       x86_64-w64-mingw32-addr2line -f -e <.debug.exe> <0x140000000 + deslocamento>
# e acrescenta a linha do .zip em dist/SHA256SUMS-<versao>.txt (rode DEPOIS do
# scripts/package_release.sh, que recria esse arquivo).
#
# Conteudo do .zip (mesma divisao do pacote de Linux):
#   DigimonWorld4-Recomp.exe   entrada: abre o launcher (packaging/windows/launcher_stub.c)
#   launcher/                  dw4_launcher.py, xiso.py, qml/, VERSION
#   python/                    Python embutivel oficial + PySide6 (so o que o launcher usa)
#   game/                      dw4_recomp.exe (sem simbolos), SDL2.dll, libepoxy-0.dll
#   README.txt  LICENSE.txt  licenses/
# Nada e instalado na maquina do jogador e o Python do pacote nao e congelado
# (PyInstaller nao faz compilacao cruzada): sao os arquivos oficiais, com versao
# e SHA-256 fixos abaixo, baixados para tools/win-deps/launcher-dl/.
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
DL="$ROOT/tools/win-deps/launcher-dl"

PY_VERSION=3.12.10
PY_ZIP="python-$PY_VERSION-embed-amd64.zip"
PY_SHA256=4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3
PYSIDE_VERSION=6.12.0           # a mesma do launcher do pacote de Linux
PYSIDE_WHL="pyside6_essentials-$PYSIDE_VERSION-cp310-abi3-win_amd64.whl"
PYSIDE_SHA256=c9a95102aa23c1f86516a30d5a9be37552d324eee7e567eb7508973f1218cb42
SHIBOKEN_WHL="shiboken6-$PYSIDE_VERSION-cp310-abi3-win_amd64.whl"
SHIBOKEN_SHA256=a1906cb8116869178c64b6bab52623bbc8b4ceed7b30bfc3c9aa0f6e67f6438e

die() { echo "package_win: ERRO: $*" >&2; exit 1; }
check_sha() { echo "$1  $2" | sha256sum -c --quiet - || die "SHA-256 nao confere: $2"; }

# ── 0. Python e Qt de Windows ────────────────────────────────────────────────
mkdir -p "$DL"
if [ ! -f "$DL/$PY_ZIP" ]; then
    curl -fsSL -o "$DL/$PY_ZIP" "https://www.python.org/ftp/python/$PY_VERSION/$PY_ZIP"
fi
if [ ! -f "$DL/$PYSIDE_WHL" ] || [ ! -f "$DL/$SHIBOKEN_WHL" ]; then
    PIP="$ROOT/.venv/bin/python"; [ -x "$PIP" ] || PIP=python3
    "$PIP" -m pip download -q --no-deps --only-binary=:all: --platform win_amd64 \
        --python-version 3.12 --implementation cp -d "$DL" \
        "PySide6_Essentials==$PYSIDE_VERSION" "shiboken6==$PYSIDE_VERSION"
fi
check_sha "$PY_SHA256" "$DL/$PY_ZIP"
check_sha "$PYSIDE_SHA256" "$DL/$PYSIDE_WHL"
check_sha "$SHIBOKEN_SHA256" "$DL/$SHIBOKEN_WHL"

# ── 1. Jogo ──────────────────────────────────────────────────────────────────
# Build de release: sem DW4_PROJECT_ROOT e sem caminhos da maquina de build nas
# informacoes de depuracao.
WIN_BUILD_DIR="$BUILD" DW4_CMAKE_ARGS="-DDW4_RELEASE=ON" scripts/build_win.sh > "$ROOT/build-win-release.out" 2>&1 ||
    { tail -20 "$ROOT/build-win-release.out" >&2; die "o build falhou (build-win-release.out)"; }
rm -f "$ROOT/build-win-release.out"
SDL2_ROOT="$(ls -d "$ROOT"/tools/win-deps/SDL2-*/ | sort -V | tail -1)"
EPOXY_ROOT="$(ls -d "$ROOT"/tools/win-deps/epoxy-[0-9]*/ | sort -V | tail -1)"

rm -rf "$PKG" "$PKG.zip"
mkdir -p "$PKG/game" "$PKG/licenses" "$PKG/launcher/qml" "$PKG/python/Lib/site-packages"
cp "$BUILD/dw4_recomp.exe" "$DIST/$NAME.debug.exe"
"$TRIPLET-strip" -o "$PKG/game/dw4_recomp.exe" "$BUILD/dw4_recomp.exe"
cp "$SDL2_ROOT/$TRIPLET/bin/SDL2.dll" "$BUILD/libepoxy-0.dll" "$PKG/game/"

# ── 2. Launcher ──────────────────────────────────────────────────────────────
cp launcher/dw4_launcher.py launcher/xiso.py "$PKG/launcher/"
cp launcher/qml/*.qml "$PKG/launcher/qml/"
echo "$VERSION" > "$PKG/launcher/VERSION"

python3 -m zipfile -e "$DL/$PY_ZIP" "$PKG/python"
python3 -m zipfile -e "$DL/$SHIBOKEN_WHL" "$PKG/python/Lib/site-packages"
python3 -m zipfile -e "$DL/$PYSIDE_WHL" "$PKG/python/Lib/site-packages"
# O Python embutivel so enxerga o que este arquivo lista.
printf 'python312.zip\r\n.\r\nLib\\site-packages\r\n' > "$PKG/python/python312._pth"

# Do PySide6 fica so o que o launcher usa (QtCore, QtGui, QtQml, QtQuick e os
# modulos de que eles dependem; controles no estilo Basic). O resto sao
# ferramentas de desenvolvimento, outros estilos e modulos que nao importamos.
(
    cd "$PKG/python/Lib/site-packages"
    rm -rf ./*.dist-info
    cd PySide6
    rm -f ./*.exe ./*.lib ./*.pyi opengl32sw.dll
    rm -rf translations resources include lib glue doc metatypes typesystems scripts support \
           plugins/sqldrivers plugins/designer plugins/qmltooling qml/QtTest
    for m in Concurrent DBus Designer Help LabsStyleKit OpenGLWidgets PrintSupport QmlFeatures \
             QuickControls2 QuickTest QuickWidgets Sql Svg SvgWidgets Test UiTools Widgets Xml; do
        rm -f "Qt$m.pyd"
    done
    rm -f Qt6Designer*.dll Qt6Help.dll Qt6UiTools.dll Qt6Widgets.dll Qt6OpenGLWidgets.dll \
          Qt6PrintSupport.dll Qt6Sql.dll Qt6Test.dll Qt6QuickTest.dll Qt6QuickWidgets.dll \
          Qt6SvgWidgets.dll Qt6Concurrent.dll Qt6DBus.dll Qt6Xml.dll Qt6QmlCompiler.dll \
          Qt6LabsStyleKit*.dll Qt6Lottie*.dll Qt6QuickParticles.dll Qt6QuickTimeline*.dll \
          Qt6QuickVectorImage*.dll
    for st in FluentWinUI3 Imagine Material Universal Fusion; do
        rm -f Qt6QuickControls2$st*.dll
        rm -rf "qml/QtQuick/Controls/$st"
    done
)

# Entrada do pacote, com o icone do projeto (um .ico pode embutir o PNG de 256).
W="$(mktemp -d)"
python3 - packaging/dw4-recomp.png "$W/app.ico" <<'PY'
import struct, sys
png = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", png[16:24])
assert w <= 256 and h <= 256, "o icone tem de ter ate 256x256"
open(sys.argv[2], "wb").write(struct.pack("<HHH", 0, 1, 1) +
    struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(png), 22) + png)
PY
printf '1 ICON "%s"\n' "$W/app.ico" > "$W/app.rc"
"$TRIPLET-windres" "$W/app.rc" -O coff -o "$W/app.res"
"$TRIPLET-gcc" -O2 -municode -mwindows -static -o "$PKG/DigimonWorld4-Recomp.exe" \
    packaging/windows/launcher_stub.c "$W/app.res"
"$TRIPLET-strip" "$PKG/DigimonWorld4-Recomp.exe"
rm -rf "$W"

# ── 3. Textos e licencas ─────────────────────────────────────────────────────
sed -e "s|@VERSION@|$VERSION|g" -e "s|@URL@|$URL|g" packaging/windows/README.txt > "$PKG/README.txt"
cp LICENSE "$PKG/LICENSE.txt"
cp xboxrecomp/LICENSE "$PKG/licenses/xboxrecomp-MIT.txt"
cp xboxrecomp/NOTICE "$PKG/licenses/xboxrecomp-NOTICE.txt"
cp xboxrecomp/LICENSES/LGPL-2.1.txt "$PKG/licenses/LGPL-2.1.txt"
cp "$SDL2_ROOT/LICENSE.txt" "$PKG/licenses/SDL2-zlib.txt"
cp "$EPOXY_ROOT/COPYING" "$PKG/licenses/libepoxy-MIT.txt"
cp "$PKG/python/LICENSE.txt" "$PKG/licenses/Python-PSF.txt"
cat > "$PKG/licenses/MinGW-w64.txt" <<'EOT'
game\dw4_recomp.exe and DigimonWorld4-Recomp.exe are built with GCC for
MinGW-w64 and statically link:
  - the MinGW-w64 runtime and winpthreads (https://www.mingw-w64.org/),
    under their permissive licenses (ZPL 2.1 / MIT / BSD-style);
  - libgcc, under the GPL-3.0 with the GCC Runtime Library Exception, which
    allows distribution of the compiled program under any terms.
SDL2.dll is the unmodified official build from https://github.com/libsdl-org/SDL
(zlib license, see SDL2-zlib.txt); it may be replaced by another SDL 2 build.
libepoxy-0.dll is libepoxy built unmodified from https://github.com/anholt/libepoxy
(MIT license, see libepoxy-MIT.txt).
EOT
cat > "$PKG/licenses/Qt-PySide6.txt" <<'EOT'
The launcher uses Qt 6 and PySide6 (Qt for Python), (c) The Qt Company Ltd.,
under the GNU Lesser General Public License v3 (LGPL-3.0). The Qt libraries
are the unmodified official builds from https://pypi.org/project/PySide6-Essentials/
and are shipped as separate files in python\Lib\site-packages\PySide6, so you
may replace them with your own builds. Source code: https://download.qt.io/
License text: https://www.gnu.org/licenses/lgpl-3.0.html
The python\ folder is the unmodified official "embeddable package" of CPython
from https://www.python.org/ (PSF License, see Python-PSF.txt).
EOT

# ── Barreiras ────────────────────────────────────────────────────────────────
BAD="$(find "$PKG" -type f \( -iname '*.xbe' -o -iname '*.afs' -o -iname '*.igb' -o -iname '*.iso' \
        -o -iname '*.img' -o -iname '*.bmp' \) | head -5)"
[ -z "$BAD" ] || die "arquivo do jogo ou de save dentro do pacote: $BAD"

for exe in "$PKG/game/dw4_recomp.exe" "$PKG/DigimonWorld4-Recomp.exe"; do
    LEAK="$(grep -a -c -F -e "$ROOT" -e "$HOME" "$exe" || true)"
    [ "$LEAK" = "0" ] || die "$(basename "$exe") contem caminhos da maquina de build ($LEAK ocorrencias)"
done

DLLS="$("$TRIPLET-objdump" -p "$PKG/game/dw4_recomp.exe" | sed -n 's/.*DLL Name: //p' | sort -u)"
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
# Um arquivo de checksums por versao, dividido com scripts/package_release.sh
# (que o recria: rode o do Linux primeiro e este depois).
(cd "$DIST" && { grep -v -F "$NAME.zip" "SHA256SUMS-$VERSION.txt" 2>/dev/null || true; } > .sums.tmp &&
    sha256sum "$NAME.zip" >> .sums.tmp && mv .sums.tmp "SHA256SUMS-$VERSION.txt" && cat "SHA256SUMS-$VERSION.txt")
echo "DLLs importadas pelo jogo: $(echo $DLLS)"
echo "pacote: $PKG.zip ($(du -h "$PKG.zip" | cut -f1); extraido: $(du -sh "$PKG" | cut -f1))"
echo "simbolos (nao publicar): $DIST/$NAME.debug.exe"
