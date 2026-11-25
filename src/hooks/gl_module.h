/**
 * Interface entre o host (gl_host.c, dentro do executavel) e um renderizador
 * recarregavel:
 *     nv2a_gl_backend.c -> libdw4_gl.so   (OpenGL 3.3)
 *     nv2a_vk_backend.c -> libdw4_vk.so   (Vulkan)
 * O host escolhe qual carregar (DW4_RENDERER, ver gl_host.c).
 *
 * clear/draw/flip/present_sync vem todas da thread NV2A. Um modulo com
 * needs_gl recebe o contexto OpenGL do host ja corrente; no reload o host
 * destroi esse contexto (e com ele todos os objetos GL) e cria outro. Um
 * modulo sem needs_gl e dono do proprio dispositivo: init() o cria (na thread
 * que carrega o modulo) e shutdown() o destroi antes de um reload.
 * Nenhum modulo guarda nada que precise sobreviver a um reload: a copia nova
 * da biblioteca comeca com as variaveis estaticas zeradas.
 */
#ifndef DW4_GL_MODULE_H
#define DW4_GL_MODULE_H

#include "nv2a_backend.h"

#define DW4_GL_MODULE_ABI 3

typedef struct {
    int abi;
    const char *name;           /* "OpenGL", "Vulkan": aparece no log */
    int needs_gl;               /* 1: usa o contexto EGL do host */
    int (*init)(void);          /* NULL ou devolve 0 se o back end nao sobe */
    void (*shutdown)(void);     /* NULL ou libera tudo o que init criou */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    void (*present_sync)(void);
} Dw4GlModule;

/* Exportado por libdw4_gl.so e por libdw4_vk.so. Com DW4_STATIC_RENDERERS
 * (Windows) os dois renderizadores ficam dentro do executavel e a entrada de
 * cada um ganha um nome proprio. */
#if defined(DW4_STATIC_RENDERERS)
const Dw4GlModule *dw4_gl_module_gl(void);
const Dw4GlModule *dw4_gl_module_vk(void);
#else
const Dw4GlModule *dw4_gl_module(void);
#endif

#endif /* DW4_GL_MODULE_H */
