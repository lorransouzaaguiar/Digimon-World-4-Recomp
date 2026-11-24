cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Digimon World 4 (Xbox, NTSC-U) - port nativo Linux por recompilacao estatica
#
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build build -j$(nproc)
#   RECOMP_KEYBOARD=1 ./build/dw4_recomp            (ou ./run.sh)
#
# Runtime: xboxrecomp (./xboxrecomp) -- SDL2 + OpenGL 3.3 (libepoxy) + POSIX.
# Nada de Win32/DirectX: no Linux o toolkit compila apenas as camadas POSIX.
# ============================================================================

project(dw4_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(WIN32 OR APPLE)
    message(FATAL_ERROR "Este projeto e exclusivamente Linux (SDL2 + OpenGL).")
endif()

# ── Toolkit xboxrecomp ───────────────────────────────────────────────────────
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Raiz do toolkit sp00nznet/xboxrecomp")

# O verificador de ABI precisa ser definido para o jogo E para o toolkit
# (docs/pipeline/05-runtime.md), por isso vem antes do add_subdirectory.
option(RECOMP_ABI_CHECK "Loga violacoes de ABI em cada chamada recompilada" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK=1)
endif()

# Build de diagnostico: AddressSanitizer no host (toolkit + hooks + main), nao
# no codigo gerado. Use um diretorio separado:
#   cmake -S . -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang -DDW4_ASAN=ON
option(DW4_ASAN "AddressSanitizer no codigo do host (toolkit, hooks, main)" OFF)
if(DW4_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Codigo gerado pelo recompilador (src/recompiled) ─────────────────────────
# CONFIGURE_DEPENDS: rodar o recompilador de novo pode criar/remover chunks.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled/*.c")

set(DW4_GEN_OPT "-O1" CACHE STRING "Nivel de otimizacao do codigo gerado")

add_library(dw4_gen OBJECT ${RECOMP_GEN_SOURCES})
target_include_directories(dw4_gen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled)
# -fno-strict-aliasing / -fwrapv: o codigo gerado reinterpreta memoria do
# guest com larguras diferentes e depende de aritmetica x86 com wrap.
# -g0: 2,2 M de linhas; os simbolos sub_XXXXXXXX ja bastam para backtraces.
target_compile_options(dw4_gen PRIVATE
    ${DW4_GEN_OPT} -g0 -w
    -fno-strict-aliasing -fwrapv -fno-stack-protector)
if(DW4_ASAN)
    # 2,2 M de linhas instrumentadas seriam lentas demais, e o codigo gerado
    # so toca memoria do guest (fora do alcance do ASan de qualquer forma).
    target_compile_options(dw4_gen PRIVATE -fno-sanitize=address)
endif()

# ── Executavel ───────────────────────────────────────────────────────────────
file(GLOB DW4_HOOK_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/*.c")
# O renderizador vai numa biblioteca a parte (recarga a quente, ver gl_host.c).
list(FILTER DW4_HOOK_SOURCES EXCLUDE REGEX "nv2a_(gl|vk)_backend\\.c$")

add_executable(dw4_recomp
    src/main.c
    ${DW4_HOOK_SOURCES}
    $<TARGET_OBJECTS:dw4_gen>
)

target_include_directories(dw4_recomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src/input
)

target_compile_options(dw4_recomp PRIVATE
    -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -fwrapv)

# Build de release (scripts/package_release.sh): nenhum caminho da maquina de
# build fica no binario, e o jogo e procurado em game_data/ ao lado dele.
option(DW4_RELEASE "Build para distribuicao (sem caminhos da maquina de build)" OFF)
if(DW4_RELEASE)
    add_compile_options(-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.)
else()
    target_compile_definitions(dw4_recomp PRIVATE
        DW4_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DW4_EPOXY REQUIRED epoxy)
target_include_directories(dw4_recomp PRIVATE ${DW4_EPOXY_INCLUDE_DIRS})

target_link_libraries(dw4_recomp PRIVATE
    xboxrecomp SDL2::SDL2 Threads::Threads m ${CMAKE_DL_LIBS}
    ${DW4_EPOXY_LINK_LIBRARIES})

# -rdynamic: backtrace_symbols() resolve o RIP do host de volta para
# sub_XXXXXXXX, ou seja, para a funcao do guest que falhou.
target_link_options(dw4_recomp PRIVATE -rdynamic)

# ── Renderizador recarregavel ────────────────────────────────────────────────
# libdw4_gl.so fica ao lado do executavel. Os simbolos do toolkit e dos hooks
# que ele usa vem do executavel (-rdynamic). Recompilar so isto com o jogo
# aberto troca o renderizador no proximo flip:
#   cmake --build build --target dw4_gl
add_library(dw4_gl MODULE src/hooks/nv2a_gl_backend.c)
set_target_properties(dw4_gl PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    C_VISIBILITY_PRESET hidden)
target_include_directories(dw4_gl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src
    ${DW4_EPOXY_INCLUDE_DIRS})
target_compile_options(dw4_gl PRIVATE
    -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -O2)
target_link_libraries(dw4_gl PRIVATE ${DW4_EPOXY_LINK_LIBRARIES} m)
add_dependencies(dw4_recomp dw4_gl)

# ── Renderizador Vulkan (segunda API grafica, escolhida no launcher) ─────────
# libdw4_vk.so, mesma interface e mesma recarga a quente do OpenGL:
#   cmake --build build --target dw4_vk
# Cabecalhos: third_party/Vulkan-Headers (submodulo fixado), para nao depender
# do pacote vulkan-headers do sistema. Em tempo de execucao liga em
# libvulkan.so.1 (o loader, que vem com qualquer driver Vulkan); se ela nao
# existir o dlopen falha e o host cai no OpenGL. Shaders: GLSL -> SPIR-V como
# vetor C, com glslc (pacote shaderc), como nos ports de referencia.
option(DW4_VULKAN "Compila o renderizador Vulkan (libdw4_vk.so)" ON)
if(DW4_VULKAN)
    set(DW4_VK_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include)
    find_program(DW4_GLSLC glslc)
    find_library(DW4_VULKAN_LIB NAMES vulkan libvulkan.so.1)
    if(NOT EXISTS ${DW4_VK_HEADERS}/vulkan/vulkan_core.h)
        message(WARNING "Vulkan desligado: falta o submodulo "
            "(git submodule update --init third_party/Vulkan-Headers)")
    elseif(NOT DW4_GLSLC)
        message(WARNING "Vulkan desligado: falta o glslc (Arch/CachyOS: pacman -S shaderc)")
    elseif(NOT DW4_VULKAN_LIB)
        message(WARNING "Vulkan desligado: falta a libvulkan (pacman -S vulkan-icd-loader)")
    else()
        set(DW4_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/shaders)
        set(DW4_SPV_DIR ${CMAKE_BINARY_DIR}/shaders)
        file(MAKE_DIRECTORY ${DW4_SPV_DIR})
        set(DW4_SPV_OUT)
        foreach(sh nv2a_vk.vert nv2a_vk.frag)
            add_custom_command(
                OUTPUT ${DW4_SPV_DIR}/${sh}.inc
                COMMAND ${DW4_GLSLC} --target-env=vulkan1.0 -O -mfmt=c
                        ${DW4_SHADER_DIR}/${sh} -o ${DW4_SPV_DIR}/${sh}.inc
                DEPENDS ${DW4_SHADER_DIR}/${sh} ${DW4_SHADER_DIR}/nv2a_ubo.glsl
                COMMENT "glslc ${sh}")
            list(APPEND DW4_SPV_OUT ${DW4_SPV_DIR}/${sh}.inc)
        endforeach()

        add_library(dw4_vk MODULE src/hooks/nv2a_vk_backend.c ${DW4_SPV_OUT})
        set_target_properties(dw4_vk PROPERTIES
            PREFIX "lib"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            C_VISIBILITY_PRESET hidden)
        target_include_directories(dw4_vk PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks
            ${XBOXRECOMP_DIR}/src/kernel
            ${XBOXRECOMP_DIR}/src
            ${DW4_VK_HEADERS}
            ${DW4_SPV_DIR})
        target_compile_options(dw4_vk PRIVATE
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
            -fno-strict-aliasing -O2)
        target_link_libraries(dw4_vk PRIVATE ${DW4_VULKAN_LIB} m)
        add_dependencies(dw4_recomp dw4_vk)
    endif()
endif()
