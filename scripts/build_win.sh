#!/usr/bin/env bash
# Compila o port para Windows (x86-64) a partir do Linux, com o MinGW-w64.
#
#   scripts/build_win.sh              # -> build-win/dw4_recomp.exe
#   scripts/build_win.sh --clean      # apaga build-win/ antes
#   JOBS=8 scripts/build_win.sh
#   WIN_BUILD_DIR=... DW4_CMAKE_ARGS="-DDW4_RELEASE=ON" scripts/build_win.sh   # como o package_win.sh faz
#
# Precisa de: x86_64-w64-mingw32-gcc (Arch/CachyOS: sudo pacman -S --needed mingw-w64-gcc),
# cmake, glslc (pacote shaderc) e o codigo gerado em src/recompiled/ (scripts/regen.sh).
#
# O que o script traz sozinho para tools/win-deps/ (ignorado pelo git):
#   - SDL2 de desenvolvimento para MinGW (pacote oficial, versao e SHA-256 fixos abaixo);
#   - a biblioteca de importacao da vulkan-1.dll, gerada dos cabecalhos em
#     third_party/Vulkan-Headers (dispensa o SDK do Vulkan);
#   - a libepoxy (OpenGL), compilada do codigo-fonte oficial com o meson do
#     .venv (versao e SHA-256 fixos abaixo).
#
# A compilacao nao para no primeiro erro (-k 0): o log completo fica em
# build-win/build.log, para servir de lista do que falta portar.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export PATH="$ROOT/.venv/bin:$PATH"          # ninja do .venv

TRIPLET="${MINGW_TRIPLET:-x86_64-w64-mingw32}"
BUILD="${WIN_BUILD_DIR:-$ROOT/build-win}"
DEPS="$ROOT/tools/win-deps"
JOBS="${JOBS:-$(nproc)}"

SDL2_VERSION=2.32.10
SDL2_SHA256=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-devel-$SDL2_VERSION-mingw.tar.gz"

EPOXY_VERSION=1.5.10
EPOXY_SHA256=a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15
EPOXY_URL="https://github.com/anholt/libepoxy/archive/refs/tags/$EPOXY_VERSION.tar.gz"

die() { echo "build_win: $*" >&2; exit 1; }

[ "${1:-}" = "--clean" ] && rm -rf "$BUILD"

# ── Pre-requisitos ───────────────────────────────────────────────────────────
command -v "$TRIPLET-gcc" >/dev/null ||
    die "falta o $TRIPLET-gcc (Arch/CachyOS: sudo pacman -S --needed mingw-w64-gcc)"
command -v "$TRIPLET-dlltool" >/dev/null || die "falta o $TRIPLET-dlltool (binutils do MinGW)"
command -v cmake >/dev/null || die "falta o cmake"
command -v glslc >/dev/null || die "falta o glslc (Arch/CachyOS: sudo pacman -S --needed shaderc)"
[ -f src/recompiled/recomp_funcs.h ] ||
    die "falta o codigo gerado em src/recompiled/ (rode scripts/regen.sh)"
[ -f third_party/Vulkan-Headers/include/vulkan/vulkan_core.h ] ||
    die "falta o submodulo: git submodule update --init third_party/Vulkan-Headers"
git -C xboxrecomp apply --reverse --check "$ROOT/patches/xboxrecomp-dw4.patch" 2>/dev/null ||
    die "o toolkit esta sem os patches do projeto (rode scripts/setup.sh)"

# ── SDL2 para MinGW ──────────────────────────────────────────────────────────
SDL2_ROOT="$DEPS/SDL2-$SDL2_VERSION/$TRIPLET"
if [ ! -f "$SDL2_ROOT/lib/cmake/SDL2/sdl2-config.cmake" ]; then
    mkdir -p "$DEPS"
    TGZ="$DEPS/SDL2-devel-$SDL2_VERSION-mingw.tar.gz"
    echo "build_win: baixando SDL2 $SDL2_VERSION (MinGW)"
    curl -fsSL -o "$TGZ" "$SDL2_URL"
    echo "$SDL2_SHA256  $TGZ" | sha256sum -c --quiet - ||
        { rm -f "$TGZ"; die "SHA-256 do SDL2 nao confere"; }
    tar -xzf "$TGZ" -C "$DEPS"
    rm -f "$TGZ"
fi

# ── Biblioteca de importacao da vulkan-1.dll ─────────────────────────────────
# Em x86-64 os nomes exportados nao tem decoracao, entao a lista de funcoes dos
# cabecalhos basta. Entradas que a DLL do jogador nao tiver (extensoes) so
# fariam falta se fossem chamadas, e o renderizador fica no Vulkan 1.0.
VK_DIR="$DEPS/vulkan"
VK_LIB="$VK_DIR/libvulkan-1.dll.a"
VK_INC="$ROOT/third_party/Vulkan-Headers/include/vulkan"
if [ ! -f "$VK_LIB" ] || [ "$VK_INC/vulkan_core.h" -nt "$VK_LIB" ]; then
    mkdir -p "$VK_DIR"
    {
        echo "LIBRARY vulkan-1.dll"
        echo "EXPORTS"
        grep -ohE 'VKAPI_CALL vk[A-Za-z0-9_]+' "$VK_INC/vulkan_core.h" "$VK_INC/vulkan_win32.h" |
            awk '{print "    " $2}' | sort -u
    } > "$VK_DIR/vulkan-1.def"
    "$TRIPLET-dlltool" -d "$VK_DIR/vulkan-1.def" -l "$VK_LIB"
fi

# ── libepoxy para MinGW ──────────────────────────────────────────────────────
EPOXY_ROOT="$DEPS/epoxy-$EPOXY_VERSION"
if [ ! -f "$EPOXY_ROOT/lib/libepoxy.dll.a" ]; then
    command -v meson >/dev/null ||
        die "falta o meson (rode scripts/setup.sh, ou .venv/bin/pip install meson)"
    command -v ninja >/dev/null || die "falta o ninja"
    W="$DEPS/epoxy-build"
    rm -rf "$W"; mkdir -p "$W"
    echo "build_win: baixando e compilando a libepoxy $EPOXY_VERSION"
    curl -fsSL -o "$W/epoxy.tar.gz" "$EPOXY_URL"
    echo "$EPOXY_SHA256  $W/epoxy.tar.gz" | sha256sum -c --quiet - ||
        die "SHA-256 da libepoxy nao confere"
    tar -xzf "$W/epoxy.tar.gz" -C "$W"
    cat > "$W/cross.txt" <<EOT
[binaries]
c = '$TRIPLET-gcc'
ar = '$TRIPLET-ar'
strip = '$TRIPLET-strip'
windres = '$TRIPLET-windres'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOT
    meson setup "$W/b" "$W/libepoxy-$EPOXY_VERSION" --cross-file "$W/cross.txt" \
        --prefix="$EPOXY_ROOT" --buildtype=release \
        -Dtests=false -Ddocs=false -Degl=no -Dglx=no -Dx11=false > "$W/meson.log" 2>&1 &&
        ninja -C "$W/b" install >> "$W/meson.log" 2>&1 ||
        { tail -20 "$W/meson.log" >&2; die "a libepoxy nao compilou"; }
    "$TRIPLET-strip" "$EPOXY_ROOT/bin/libepoxy-0.dll"
    cp "$W/libepoxy-$EPOXY_VERSION/COPYING" "$EPOXY_ROOT/COPYING"
    rm -rf "$W"
fi

# ── Configurar e compilar ────────────────────────────────────────────────────
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)

cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/xboxrecomp/cmake/mingw-w64-x86_64.cmake" \
    -DMINGW_TRIPLET="$TRIPLET" \
    -DSDL2_DIR="$SDL2_ROOT/lib/cmake/SDL2" \
    -DDW4_VULKAN_LIB="$VK_LIB" \
    -DDW4_EPOXY_ROOT="$EPOXY_ROOT" \
    ${DW4_CMAKE_ARGS:-}

KEEP=()
[ ${#GEN[@]} -gt 0 ] && KEEP=(-- -k 0)

LOG="$BUILD/build.log"
set +e
cmake --build "$BUILD" --parallel "$JOBS" "${KEEP[@]}" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

echo
if [ "$RC" -eq 0 ] && [ -f "$BUILD/dw4_recomp.exe" ]; then
    # As DLLs precisam estar ao lado do executavel para ele subir.
    cp -u "$SDL2_ROOT/bin/SDL2.dll" "$EPOXY_ROOT/bin/libepoxy-0.dll" "$BUILD/"
    echo "build_win: OK -> $BUILD/dw4_recomp.exe"
    echo "DLLs importadas:"
    "$TRIPLET-objdump" -p "$BUILD/dw4_recomp.exe" | grep 'DLL Name' | sort -u
else
    ERRS=$(grep -c -E '(error:|undefined reference)' "$LOG" || true)
    echo "build_win: FALHOU ($ERRS linhas de erro). Log completo: $LOG"
    echo "Arquivos com erro:"
    grep -E 'error:' "$LOG" | sed -E 's/:[0-9]+:[0-9]+: .*//' | sort | uniq -c | sort -rn | head -30
    exit 1
fi
