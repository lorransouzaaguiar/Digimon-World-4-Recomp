#!/usr/bin/env bash
# Monta os pacotes de release para Linux em dist/ (sem nenhum arquivo do jogo):
#
#   scripts/package_release.sh v0.1.0
#
#   dist/DigimonWorld4-Recomp-<versao>-x86_64.AppImage      um arquivo so: baixar e abrir
#   dist/DigimonWorld4-Recomp-<versao>-linux-x86_64.tar.gz  o mesmo conteudo, para quem nao tem FUSE
#   dist/SHA256SUMS-<versao>.txt
#
# O pacote traz tudo: launcher congelado (PyInstaller: Python + Qt), o jogo
# recompilado e as bibliotecas (SDL2, libepoxy, libcrypto). O jogador so
# precisa do proprio disco. Dentro do pacote:
#   AppRun  launcher/ (PyInstaller)  game/{dw4_recomp,libdw4_gl.so,libdw4_vk.so,lib/}  licenses/
#
# Precisa de: codigo gerado (scripts/regen.sh), .venv com pyinstaller e
# pysdl2-dll (scripts/setup.sh), e tools/appimagetool-x86_64.AppImage
# (baixado aqui se faltar).
#   DW4_URL=<url do projeto>   endereco escrito no README do pacote
#   JOBS=<n>                   paralelismo do build (padrao: nproc)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="${1:?uso: scripts/package_release.sh <versao, ex. v0.1.0>}"
URL="${DW4_URL:-}"
NAME="DigimonWorld4-Recomp-$VERSION"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
APPDIR="$DIST/$NAME-linux-x86_64"
# Python portatil (python-build-standalone, glibc antiga) so para congelar o
# launcher: o Python do CachyOS amarraria o pacote a glibc do dia.
PY="$ROOT/tools/pyfreeze/bin/python3"
PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261123/cpython-3.12.16%2B20261123-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
PY_SHA="c85811d35971d06612585a057db3b71806378df6ea8476275fb16aa5c9fe56a0"
GLIBC_MAX="2.38"          # o build falha se algo no pacote pedir mais que isto
TOOL="$ROOT/tools/appimagetool-x86_64.AppImage"

ls src/recompiled/*.c >/dev/null 2>&1 || { echo "Sem codigo gerado. Rode antes: scripts/regen.sh" >&2; exit 1; }
scripts/dist_prune.sh "$VERSION"      # regra do projeto: dist/ so guarda a versao que esta sendo criada
mkdir -p "$ROOT/tools"
if [ ! -x "$PY" ]; then
    curl -fsSL -o "$ROOT/tools/python-standalone.tar.gz" "$PY_URL"
    echo "$PY_SHA  $ROOT/tools/python-standalone.tar.gz" | sha256sum -c - >/dev/null
    rm -rf "$ROOT/tools/pyfreeze"; mkdir "$ROOT/tools/pyfreeze"
    tar -xzf "$ROOT/tools/python-standalone.tar.gz" -C "$ROOT/tools/pyfreeze" --strip-components=1
fi
"$PY" -c "import PyInstaller, sdl2dll, PySide6.QtQuick" 2>/dev/null ||
    "$PY" -m pip install -q --no-warn-script-location -r launcher/requirements.txt pyinstaller pysdl2-dll
[ -d .venv/bin ] && export PATH="$ROOT/.venv/bin:$PATH"      # ninja
if [ ! -x "$TOOL" ]; then
    mkdir -p "$ROOT/tools"
    curl -fsSL -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi

# ── 1. jogo ──────────────────────────────────────────────────────────────────
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDW4_RELEASE=ON \
      -DCMAKE_C_COMPILER="${CC:-clang}" >/dev/null
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"

rm -rf "$APPDIR"; mkdir -p "$APPDIR/game/lib" "$APPDIR/licenses"
cp "$BUILD/dw4_recomp" "$BUILD/libdw4_gl.so" "$BUILD/libdw4_vk.so" "$APPDIR/game/"
# So a informacao de depuracao sai: os simbolos sub_XXXXXXXX ficam (relatorio
# de crash, e o renderizador resolve simbolos do executavel).
strip --strip-debug "$APPDIR/game/dw4_recomp" "$APPDIR/game/libdw4_gl.so" "$APPDIR/game/libdw4_vk.so"
# libvulkan (o loader) NAO entra no pacote: tem de ser a do sistema, a mesma do
# driver. Sem ela o renderizador Vulkan nao carrega e o jogo abre com OpenGL.

# Bibliotecas do jogo. SDL2 vem da roda pysdl2-dll (SDL2 de verdade, compilada
# para glibc antiga); a do CachyOS e o sdl2-compat, que exige SDL3 e glibc nova.
cp "$("$PY" -c 'import sdl2dll; print(sdl2dll.get_dllpath())')/libSDL2-2.0.so" "$APPDIR/game/lib/libSDL2-2.0.so.0"
while read -r soname path; do
    case "$soname" in
        libcrypto.so.*|libepoxy.so.*|libbrotli*|libzstd.so.*|libz.so.*) cp -L "$path" "$APPDIR/game/lib/$soname" ;;
    esac
done < <(ldd "$BUILD/dw4_recomp" | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }')
ls "$APPDIR/game/lib"/libcrypto.so.* "$APPDIR/game/lib"/libepoxy.so.* >/dev/null

# ── 2. launcher congelado ────────────────────────────────────────────────────
WORK="$BUILD/pyinstaller"
echo "$VERSION" > "$BUILD/VERSION"
"$PY" -m PyInstaller --noconfirm --clean --log-level WARN --onedir --name dw4-launcher \
    --distpath "$WORK/dist" --workpath "$WORK/work" --specpath "$WORK" \
    --paths "$ROOT/launcher" --hidden-import xiso --hidden-import updater --hidden-import xsave \
    --add-data "$ROOT/launcher/qml:qml" --add-data "$BUILD/VERSION:." \
    --add-data "$ROOT/packaging/dw4-recomp.png:." \
    "$ROOT/launcher/dw4_launcher.py"
cp -r "$WORK/dist/dw4-launcher" "$APPDIR/launcher"

# O PyInstaller copia tambem bibliotecas do sistema de build (GTK, glib, X11,
# fontconfig...). Essas tem de vir do sistema de quem joga: as do CachyOS pedem
# glibc recente e todo desktop Linux ja as tem. Ficam so o Python, o Qt e as
# pequenas xcb-util / xkbcommon que o Qt usa e nem toda distribuicao instala.
INT="$APPDIR/launcher/_internal"
rm -f "$INT"/PySide6/Qt/plugins/platformthemes/libqgtk3.so
for f in "$INT"/lib*.so*; do
    case "$(basename "$f")" in
        libpython*|libshiboken*|libpyside*|libxcb-cursor*|libxcb-icccm*|libxcb-image*|libxcb-keysyms*|\
        libxcb-render-util*|libxcb-util*|libxkbcommon*) ;;
        *) rm -f "$f" ;;
    esac
done
# Estilos de controles e traducoes do Qt que o launcher nao usa (estilo Basic).
for st in FluentWinUI3 Imagine Material Universal Fusion designer; do
    rm -rf "$INT/PySide6/Qt/qml/QtQuick/Controls/$st"
    rm -f "$INT"/PySide6/Qt/lib/libQt6QuickControls2${st}*.so*
done
rm -rf "$INT/PySide6/Qt/translations"

# ── 3. AppDir ────────────────────────────────────────────────────────────────
cp packaging/AppRun "$APPDIR/AppRun"
cp packaging/dw4-recomp.desktop packaging/dw4-recomp.png "$APPDIR/"
ln -s dw4-recomp.png "$APPDIR/.DirIcon"
ln -s AppRun "$APPDIR/DigimonWorld4-Recomp"
GLIBC="$(find "$APPDIR" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null |
         grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)"
sed -e "s|@VERSION@|$VERSION|g" -e "s|@URL@|$URL|g" -e "s|@GLIBC@|${GLIBC#GLIBC_}|g" \
    packaging/README.txt > "$APPDIR/README.txt"
[ -f LICENSE ] && cp LICENSE "$APPDIR/LICENSE.txt"
cp xboxrecomp/LICENSE "$APPDIR/licenses/xboxrecomp-MIT.txt"
cp xboxrecomp/NOTICE "$APPDIR/licenses/xboxrecomp-NOTICE.txt"
cp xboxrecomp/LICENSES/LGPL-2.1.txt "$APPDIR/licenses/LGPL-2.1.txt"
for d in openssl zlib zstd brotli libepoxy sdl2 python; do
    f="$(ls /usr/share/licenses/$d/* 2>/dev/null | head -1 || true)"
    [ -n "$f" ] && cp -L "$f" "$APPDIR/licenses/$d-$(basename "$f").txt"
done
cat > "$APPDIR/licenses/Qt-PySide6.txt" <<'EOT'
The launcher uses Qt 6 and PySide6 (Qt for Python), (c) The Qt Company Ltd,
under the GNU Lesser General Public License v3 (LGPL-3.0). The Qt libraries
are shipped unmodified as separate shared objects in launcher/_internal/ and
can be replaced. Sources: https://download.qt.io/ and https://pypi.org/project/PySide6/
SDL2 (zlib license) comes from https://pypi.org/project/pysdl2-dll/.
EOT

ver_gt() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" != "$2" ]; }
if ver_gt "${GLIBC#GLIBC_}" "$GLIBC_MAX"; then
    echo "ERRO: algo no pacote pede glibc ${GLIBC#GLIBC_} (limite $GLIBC_MAX):" >&2
    find "$APPDIR" -type f \( -name '*.so*' -o -perm -u+x \) | while read -r f; do
        v="$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)"
        [ -n "$v" ] && ver_gt "${v#GLIBC_}" "$GLIBC_MAX" && echo "  ${v#GLIBC_}  ${f#$APPDIR/}" >&2
    done
    exit 1
fi

# Barreiras: nada do jogo e nenhum caminho da maquina de build no pacote.
if find "$APPDIR" -iname '*.xbe' -o -iname '*.afs' -o -iname '*.igb' -o -iname '*.iso' | grep -q .; then
    echo "ERRO: arquivo do jogo dentro do pacote" >&2; exit 1
fi
if grep -rl -a -e "$HOME" "$APPDIR" | grep -q .; then
    echo "ERRO: caminho da maquina de build dentro do pacote:" >&2
    grep -rl -a -e "$HOME" "$APPDIR" | head >&2; exit 1
fi

# ── 4. pacotes ───────────────────────────────────────────────────────────────
rm -f "$DIST/$NAME"-*.AppImage "$DIST/$NAME"-*.tar.gz
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$APPDIR" "$DIST/$NAME-x86_64.AppImage" >/dev/null 2>&1 ||
    { echo "ERRO: appimagetool falhou" >&2; exit 1; }
tar -C "$DIST" --owner=0 --group=0 -czf "$APPDIR.tar.gz" "$(basename "$APPDIR")"
(cd "$DIST" && sha256sum "$NAME-x86_64.AppImage" "$(basename "$APPDIR").tar.gz" > "SHA256SUMS-$VERSION.txt" && cat "SHA256SUMS-$VERSION.txt")
du -h "$DIST/$NAME-x86_64.AppImage" "$APPDIR.tar.gz" | awk '{print "pacote:", $2, "(" $1 ")"}'
echo "glibc minima: ${GLIBC#GLIBC_}"
